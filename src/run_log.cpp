#include "run_log.hpp"

#include <format>
#include <utility>

namespace egraph {

namespace {

std::string joined(const std::vector<std::string>& words) {
    std::string found;
    for (const auto& word : words) {
        found += found.empty() ? word : " " + word;
    }
    return found;
}

std::string counted(std::int64_t count, std::string_view one, std::string_view many) {
    return std::format("{} {}", count, count == 1 ? one : many);
}

} // namespace

RunEvents::RunEvents(std::string run, std::vector<RunStep> steps)
    : run_{std::move(run)}, steps_{std::move(steps)}, times_(steps_.size()) {}

log::Event RunEvents::step_event(std::size_t step, std::string kind, std::string message,
                                 double now) const {
    return {.time = now,
            .run = run_,
            .kind = std::move(kind),
            .message = std::format("{}: {}", steps_.at(step).cpv, message),
            .priority = 6,
            .fields = {{.name = "step", .value = static_cast<std::int64_t>(step + 1)},
                       {.name = "cpv", .value = steps_.at(step).cpv}}};
}

log::Event RunEvents::started(const RunStart& start, double now) {
    start_ = now;
    command_ = start.command;
    std::int64_t merges = 0;
    std::int64_t uninstalls = 0;
    for (const auto& step : steps_) {
        (step.uninstall ? uninstalls : merges) += 1;
    }
    auto message =
        std::format("egraph {}: {}, {}", start.command, counted(merges, "merge", "merges"),
                    counted(uninstalls, "uninstall", "uninstalls"));
    if (!start.targets.empty()) {
        message += std::format(", for {}", joined(start.targets));
    }
    log::Event found{.time = now,
                     .run = run_,
                     .kind = "run",
                     .message = std::move(message),
                     .priority = 6,
                     .fields = {{.name = "command", .value = start.command},
                                {.name = "targets", .value = joined(start.targets)},
                                {.name = "options", .value = joined(start.options)},
                                {.name = "keep_going", .value = std::int64_t{start.keep_going}},
                                {.name = "merges", .value = merges},
                                {.name = "uninstalls", .value = uninstalls}}};
    if (start.jobs) {
        found.fields.push_back({.name = "jobs", .value = std::int64_t{*start.jobs}});
    }
    return found;
}

std::optional<log::Event> RunEvents::traced(std::size_t step, Traced what, double now) {
    auto& times = times_.at(step);
    switch (what) {
    case Traced::build_started:
        times.build_start = now;
        return std::nullopt;
    case Traced::merge_started:
        times.merge_start = now;
        return std::nullopt;
    case Traced::skipped:
        return std::nullopt;
    case Traced::built: {
        times.built = now;
        const auto seconds = now - times.build_start.value_or(now);
        auto found = step_event(step, "built", "built in " + log::duration(seconds), now);
        found.fields.push_back({.name = "seconds", .value = seconds});
        return found;
    }
    case Traced::merged: {
        const auto seconds = now - times.merge_start.value_or(now);
        if (steps_.at(step).uninstall) {
            ++uninstalled_;
            auto found =
                step_event(step, "uninstalled", "uninstalled in " + log::duration(seconds), now);
            found.fields.push_back({.name = "seconds", .value = seconds});
            return found;
        }
        ++merged_;
        auto found = step_event(step, "merged", "merged in " + log::duration(seconds), now);
        found.fields.push_back({.name = "seconds", .value = seconds});
        if (times.built && times.build_start) {
            found.fields.push_back(
                {.name = "build_seconds", .value = *times.built - *times.build_start});
        }
        if (times.built && times.merge_start) {
            // Merge-wait's hold between the build's end and its merge.
            found.fields.push_back({.name = "waited", .value = *times.merge_start - *times.built});
        }
        return found;
    }
    case Traced::build_failed:
    case Traced::merge_failed:
        break;
    }
    ++failed_;
    const auto since = what == Traced::build_failed ? times.build_start : times.merge_start;
    const auto& failure = times.failure;
    auto found =
        step_event(step, "failed", failure ? describe_event(*failure) : std::string{"failed"}, now);
    found.priority = 3;
    found.fields.push_back({.name = "seconds", .value = now - since.value_or(now)});
    if (failure && failure->kind == WorkerEvent::Kind::failed) {
        found.fields.push_back({.name = "phase", .value = failure->text});
        found.fields.push_back({.name = "status", .value = std::int64_t{failure->status}});
        found.fields.push_back({.name = "log", .value = failure->log});
    } else if (failure) {
        found.fields.push_back({.name = "error", .value = failure->text});
    }
    return found;
}

std::optional<log::Event> RunEvents::reported(std::size_t step, const WorkerEvent& event,
                                              double now) {
    using Kind = WorkerEvent::Kind;
    if (event.kind == Kind::failed || event.kind == Kind::error) {
        times_.at(step).failure = event;
        return std::nullopt;
    }
    if (event.kind != Kind::phase) {
        return std::nullopt;
    }
    auto found = step_event(step, "phase", event.text, now);
    found.fields.push_back({.name = "phase", .value = event.text});
    return found;
}

log::Event RunEvents::skipped(std::size_t step, const std::string& why, double now) {
    ++skipped_;
    auto found = step_event(step, "skipped", "skipped, " + why, now);
    found.priority = 4;
    found.fields.push_back({.name = "why", .value = why});
    return found;
}

log::Event RunEvents::ended(const std::optional<std::string>& error, double now) const {
    const auto seconds = now - start_;
    auto message = std::format("egraph {} {}: {} merged, {} uninstalled, {} failed, {} skipped "
                               "in {}",
                               command_, error ? "failed" : "done", merged_, uninstalled_, failed_,
                               skipped_, log::duration(seconds));
    if (error) {
        message += ": " + *error;
    }
    log::Event found{.time = now,
                     .run = run_,
                     .kind = "end",
                     .message = std::move(message),
                     .priority = error ? 3 : 6,
                     .fields = {{.name = "status", .value = std::string{error ? "failed" : "ok"}},
                                {.name = "merged", .value = merged_},
                                {.name = "uninstalled", .value = uninstalled_},
                                {.name = "failed", .value = failed_},
                                {.name = "skipped", .value = skipped_},
                                {.name = "seconds", .value = seconds}}};
    if (error) {
        found.fields.push_back({.name = "error", .value = *error});
    }
    return found;
}

log::Event handed_over(const std::string& run, const HandOver& hand_over, double now) {
    auto message = std::format("egraph {}: {}", hand_over.command, hand_over.program);
    if (!hand_over.targets.empty()) {
        message += std::format(", for {}", joined(hand_over.targets));
    }
    return {.time = now,
            .run = run,
            .kind = "run",
            .message = std::move(message),
            .priority = 6,
            .fields = {{.name = "command", .value = hand_over.command},
                       {.name = "targets", .value = joined(hand_over.targets)},
                       {.name = "program", .value = hand_over.program},
                       {.name = "argv", .value = joined(hand_over.argv)}}};
}

log::Event handed_back(const std::string& run, const HandOver& hand_over,
                       const std::expected<int, std::string>& ran, double started, double now) {
    const auto seconds = now - started;
    std::optional<std::string> error;
    if (!ran) {
        error = ran.error();
    } else if (*ran != 0) {
        error = std::format("{} exited with status {}", hand_over.program, *ran);
    }
    auto message = std::format("egraph {} {} in {}", hand_over.command, error ? "failed" : "done",
                               log::duration(seconds));
    if (error) {
        message += ": " + *error;
    }
    log::Event found{.time = now,
                     .run = run,
                     .kind = "end",
                     .message = std::move(message),
                     .priority = error ? 3 : 6,
                     .fields = {{.name = "status", .value = std::string{error ? "failed" : "ok"}},
                                {.name = "program", .value = hand_over.program},
                                {.name = "seconds", .value = seconds}}};
    if (ran) {
        found.fields.push_back({.name = "exit_status", .value = std::int64_t{*ran}});
    }
    if (error) {
        found.fields.push_back({.name = "error", .value = *error});
    }
    return found;
}

} // namespace egraph
