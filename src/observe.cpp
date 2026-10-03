#include "observe.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace egraph {

namespace {

using Stage = Schedule::Stage;

bool running(Stage stage) {
    return stage == Stage::building || stage == Stage::waiting || stage == Stage::let_through ||
           stage == Stage::merging;
}

std::uint64_t merges(const Schedule& schedule, Stage stage) {
    std::uint64_t count = 0;
    for (std::size_t step = 0; step < schedule.steps().size(); ++step) {
        if (std::holds_alternative<MergeStep>(schedule.step(step)) &&
            schedule.stage(step) == stage) {
            ++count;
        }
    }
    return count;
}

} // namespace

Observer::Observer(std::int64_t pid, std::optional<std::uint32_t> jobs, std::vector<Observed> steps)
    : pid_{pid}, jobs_{jobs}, steps_{std::move(steps)}, times_(steps_.size()) {
    for (const auto& step : steps_) {
        total_ += step.operation == "merge" ? 1U : 0U;
    }
}

void Observer::update(const Schedule& schedule, double now) {
    for (std::size_t step = 0; step < times_.size(); ++step) {
        const auto stage = schedule.stage(step);
        auto& times = times_.at(step);
        if (!times.start && stage != Stage::queued && stage != Stage::skipped) {
            times.start = now;
        }
        if (times.start && !times.built && stage != Stage::queued && stage != Stage::building) {
            times.built = now;
        }
    }
}

void Observer::phase(std::size_t step, std::string name) {
    times_.at(step).phase = std::move(name);
}

void Observer::worker(std::size_t step, std::int64_t pid) {
    times_.at(step).pid = pid;
}

void Observer::resumed(const Schedule& schedule) {
    total_ = merges(schedule, Stage::queued);
}

emerge::Snapshot Observer::snapshot(const Schedule& schedule, double now) const {
    std::uint64_t failed = 0;
    std::vector<emerge::Task> tasks;
    for (std::size_t step = 0; step < times_.size(); ++step) {
        const auto stage = schedule.stage(step);
        failed += stage == Stage::failed ? 1U : 0U;
        if (!running(stage)) {
            continue;
        }
        const auto& times = times_.at(step);
        const bool merge = std::holds_alternative<MergeStep>(schedule.step(step));
        const bool waiting = stage == Stage::waiting;
        emerge::Task task{
            .cpv = steps_.at(step).cpv,
            .root = steps_.at(step).root,
            .operation = steps_.at(step).operation,
            .kind = stage == Stage::building ? emerge::TaskKind::build : emerge::TaskKind::merge,
            .phase = waiting ? "merge-wait" : times.phase,
            .merge_wait = waiting,
            .pid = stage == Stage::building || stage == Stage::merging ? times.pid : std::nullopt,
            .start_time = times.start};
        if (times.start) {
            // Waiting for merge-wait does not count, as emerge freezes it at the build's end.
            const auto end = waiting && merge && times.built ? *times.built : now;
            task.elapsed = end - *times.start;
            if (merge) {
                task.build_elapsed = times.built.value_or(now) - *times.start;
            }
        }
        tasks.push_back(std::move(task));
    }
    std::ranges::stable_sort(tasks, [](const emerge::Task& a, const emerge::Task& b) {
        return std::pair{!a.start_time, a.start_time.value_or(0)} <
               std::pair{!b.start_time, b.start_time.value_or(0)};
    });
    return {.pid = pid_,
            .timestamp = now,
            .jobs = {.running = schedule.building(),
                     .max = jobs_,
                     .completed = merges(schedule, Stage::done),
                     .total = total_,
                     .failed = failed,
                     .merge_wait = schedule.waiting(),
                     .merges_pending = schedule.let_through()},
            .tasks = std::move(tasks)};
}

} // namespace egraph
