#include "estimate.hpp"

#include "human.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <system_error>
#include <utility>
#include <variant>

namespace egraph {

namespace {

std::string cp_of(std::string_view cpv) {
    const auto parts = split_cpv(cpv);
    return std::format("{}/{}", parts.category, parts.name);
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values.at(middle)
                                  : (values.at(middle - 1) + values.at(middle)) / 2;
}

// Its build's seconds without fetching.
double built_in(const BuildRecord& record) {
    double fetching = 0;
    for (const auto& phase : record.phases) {
        if (phase.phase == "fetch") {
            fetching += phase.seconds;
        }
    }
    return std::max(record.build_seconds - fetching, 0.0);
}

// Whether it got less of the system than it asked for: beside others on a saturated system, or
// beside others on one that said nothing of itself.
bool starved(const BuildRecord& record) {
    if (record.beside == 0) {
        return false;
    }
    if (record.system && record.system->cpu_busy) {
        return *record.system->cpu_busy >= saturated_share;
    }
    return true;
}

// The flags one has that the other lacks, either way.
std::size_t use_distance(const std::vector<std::string>& built,
                         const std::vector<std::string>& wanted) {
    std::vector<std::string_view> differing;
    std::ranges::set_symmetric_difference(built, wanted, std::back_inserter(differing));
    return differing.size();
}

// How much the wanted distfiles scale a build's time, where both sizes are known.
double distfile_scale(const BuildRecord& record, std::optional<std::uint64_t> wanted) {
    if (!wanted || *wanted == 0 || !record.distfiles_size || *record.distfiles_size == 0) {
        return 1;
    }
    return std::clamp(static_cast<double>(*wanted) / static_cast<double>(*record.distfiles_size),
                      1 / distfile_scale_limit, distfile_scale_limit);
}

} // namespace

BuildKnowledge::BuildKnowledge(const std::vector<BuildRecord>& records) {
    for (const auto& record : records) {
        if (record.outcome == BuildRecord::Outcome::merged) {
            auto& found = by_cp_[cp_of(record.cpv)];
            found.push_back(record);
        }
    }
}

std::optional<PackageEstimate> BuildKnowledge::estimate(const BuildWanted& wanted,
                                                        unsigned cores) const {
    const auto& cpv = wanted.cpv;
    const auto found = by_cp_.find(cp_of(cpv));
    if (found == by_cp_.end()) {
        return std::nullopt;
    }
    const auto& all = found->second;
    const bool same_version =
        std::ranges::any_of(all, [&](const BuildRecord& record) { return record.cpv == cpv; });
    // The latest first, then those of the nearest USE, keeping that order among equals.
    std::vector<std::reference_wrapper<const BuildRecord>> pool;
    for (const auto& record : all | std::views::reverse) {
        if (!same_version || record.cpv == cpv) {
            pool.emplace_back(record);
        }
    }
    if (wanted.use) {
        std::ranges::stable_sort(pool, std::less<>{}, [&](const BuildRecord& record) {
            return use_distance(record.use, *wanted.use);
        });
    }
    const auto chosen = pool | std::views::take(estimate_builds) | std::ranges::to<std::vector>();
    // The most cores the package kept busy in any of them.
    double most_parallel = 0;
    for (const BuildRecord& record : chosen) {
        if (record.cpu_seconds && built_in(record) > 0) {
            most_parallel = std::max(most_parallel, *record.cpu_seconds / built_in(record));
        }
    }
    PackageEstimate estimate{.builds = chosen.size(), .same_version = same_version};
    std::vector<double> seconds;
    std::vector<double> cpu;
    std::vector<double> merges;
    for (const BuildRecord& record : chosen) {
        const auto cpu_seconds = record.cpu_seconds.value_or(0);
        const bool timed = starved(record) && record.cpu_seconds && most_parallel > 0;
        const auto scale = distfile_scale(record, wanted.distfiles);
        seconds.push_back(scale * (timed ? cpu_seconds / most_parallel : built_in(record)));
        if (record.cpu_seconds) {
            cpu.push_back(scale * *record.cpu_seconds);
        }
        if (record.merge_seconds) {
            merges.push_back(*record.merge_seconds);
        }
        if (record.peak_memory) {
            estimate.peak_memory = std::max(estimate.peak_memory.value_or(0), *record.peak_memory);
        }
    }
    estimate.seconds = median(seconds);
    if (!cpu.empty()) {
        estimate.parallelism = estimate.seconds > 0 ? median(cpu) / estimate.seconds : 0;
    }
    estimate.parallelism = std::min(estimate.parallelism, static_cast<double>(cores));
    if (!merges.empty()) {
        estimate.merge_seconds = median(merges);
    }
    return estimate;
}

PlanEstimate estimate_plan(Schedule schedule, std::span<const StepCost> costs, unsigned cores,
                           std::optional<std::uint32_t> tokens,
                           std::span<const RunningStep> running) {
    struct Going {
        std::size_t step;
        bool merging;
        // Seconds left: a build's at its own pace, a merge's as they pass.
        double left;
    };
    const auto steps = schedule.steps().size();
    const std::vector<StepCost> cost_of(costs.begin(), costs.end());
    PlanEstimate estimate{.finished = std::vector<std::optional<double>>(steps)};
    for (std::size_t step = 0; step < steps; ++step) {
        if (std::holds_alternative<MergeStep>(schedule.step(step)) &&
            schedule.stage(step) != Schedule::Stage::done) {
            ++(cost_of.at(step).known ? estimate.known : estimate.unknown);
        }
    }
    std::vector<Going> going;
    for (const auto& [step, merging, elapsed] : running) {
        const auto& cost = cost_of.at(step);
        going.push_back(
            {.step = step,
             .merging = merging,
             .left = std::max((merging ? cost.merge_seconds : cost.build_seconds) - elapsed, 0.0)});
    }
    double now = 0;
    for (;;) {
        while (const auto step = schedule.next_merge()) {
            schedule.merge_started(*step);
            going.push_back(
                {.step = *step, .merging = true, .left = cost_of.at(*step).merge_seconds});
        }
        while (const auto step = schedule.next_build()) {
            if (!schedule.alone() && tokens && schedule.building() >= *tokens) {
                break;
            }
            schedule.build_started(*step);
            going.push_back(
                {.step = *step, .merging = false, .left = cost_of.at(*step).build_seconds});
        }
        std::optional<std::uint64_t> memory;
        double wanted = 0;
        for (const auto& each : going) {
            if (!each.merging) {
                const auto& cost = cost_of.at(each.step);
                wanted += cost.parallelism;
                if (cost.peak_memory) {
                    memory = memory.value_or(0) + *cost.peak_memory;
                }
            }
        }
        if (memory) {
            estimate.peak_memory = std::max(estimate.peak_memory.value_or(0), *memory);
        }
        if (going.empty()) {
            break;
        }
        // Builds slowed alike when they want more cores than there are; one that wants none
        // keeps its pace, as merges do.
        const double share = wanted > cores ? cores / wanted : 1.0;
        const auto pace = [&](const Going& each) {
            return each.merging || cost_of.at(each.step).parallelism == 0 ? 1.0 : share;
        };
        double next = std::numeric_limits<double>::infinity();
        for (const auto& each : going) {
            next = std::min(next, each.left / pace(each));
        }
        now += next;
        std::vector<Going> ended;
        std::vector<Going> left;
        for (auto& each : going) {
            each.left -= next * pace(each);
            (each.left <= 1e-9 ? ended : left).push_back(each);
        }
        going = std::move(left);
        for (const auto& each : ended) {
            if (each.merging) {
                schedule.merge_finished(each.step, true);
                estimate.finished.at(each.step) = now;
            } else {
                schedule.build_finished(each.step, true);
            }
        }
    }
    estimate.seconds = now;
    return estimate;
}

std::vector<StepFacts> step_facts(const RepositoryIndex& index, std::span<const BuildStep> steps) {
    std::map<std::pair<std::string_view, std::string_view>, std::size_t> wanted;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.subspan(i).front();
        if (!step.uninstall) {
            wanted.emplace(std::pair{std::string_view{step.cpv}, std::string_view{step.repo}}, i);
        }
    }
    std::map<std::pair<std::string_view, std::string_view>,
             std::reference_wrapper<const IndexVersion>>
        found;
    for (const auto& version : index.versions) {
        const std::pair key{index.string(version.cpv),
                            index.string(index.repositories.at(version.repository).name)};
        if (wanted.contains(key)) {
            found.emplace(key, version);
        }
    }
    std::vector<StepFacts> facts;
    facts.reserve(steps.size());
    for (const auto& step : steps) {
        const auto version =
            found.find(std::pair{std::string_view{step.cpv}, std::string_view{step.repo}});
        if (step.uninstall || version == found.end()) {
            facts.emplace_back();
            continue;
        }
        facts.push_back({.distfiles = distfiles_size(index, version->second, step.use)});
    }
    return facts;
}

std::vector<StepCost> step_costs(const BuildKnowledge& knowledge, std::span<const BuildStep> steps,
                                 unsigned cores, std::span<const StepFacts> facts) {
    std::vector<StepCost> costs;
    costs.reserve(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.subspan(i).front();
        const BuildWanted wanted{.cpv = step.cpv,
                                 .use = step.use,
                                 .distfiles = i < facts.size() ? facts.subspan(i).front().distfiles
                                                               : std::nullopt};
        const auto estimate = step.uninstall ? std::nullopt : knowledge.estimate(wanted, cores);
        if (!estimate) {
            costs.emplace_back();
            continue;
        }
        costs.push_back({.build_seconds = estimate->seconds,
                         .parallelism = estimate->parallelism,
                         .peak_memory = estimate->peak_memory,
                         .merge_seconds = estimate->merge_seconds,
                         .known = true});
    }
    return costs;
}

std::optional<std::uint32_t> recorded_jobs(const std::vector<BuildRecord>& records) {
    return records.empty() ? std::nullopt : records.back().jobs;
}

std::string estimate_line(const PlanEstimate& estimate, std::optional<std::uint32_t> jobs) {
    return std::format("\testimate\t{}\t{}\t{}\t{}\t{}", std::llround(estimate.seconds),
                       estimate.peak_memory ? std::to_string(*estimate.peak_memory) : "",
                       estimate.known, estimate.unknown, jobs ? std::to_string(*jobs) : "");
}

std::optional<EstimateLine> parse_estimate_line(std::string_view line) {
    std::vector<std::string_view> fields;
    for (const auto field : std::views::split(line, '\t')) {
        fields.emplace_back(field.begin(), field.end());
    }
    if (fields.size() != 7 || !fields.at(0).empty() || fields.at(1) != "estimate") {
        return std::nullopt;
    }
    const auto number = [](std::string_view text, auto& value) {
        const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
        return error == std::errc{} && end == text.end();
    };
    EstimateLine read;
    if (!number(fields.at(2), read.seconds) || !number(fields.at(4), read.known) ||
        !number(fields.at(5), read.unknown)) {
        return std::nullopt;
    }
    if (!fields.at(3).empty() && !number(fields.at(3), read.peak_memory.emplace())) {
        return std::nullopt;
    }
    if (!fields.at(6).empty() && !number(fields.at(6), read.jobs.emplace())) {
        return std::nullopt;
    }
    return read;
}

Countdown::Countdown(std::vector<StepCost> costs, unsigned cores,
                     std::optional<std::uint32_t> tokens)
    : costs_{std::move(costs)}, cores_{cores}, tokens_{tokens} {}

void Countdown::traced(std::size_t step, Traced what, double now) {
    switch (what) {
    case Traced::build_started:
    case Traced::merge_started:
        running_.insert_or_assign(step,
                                  Since{.merging = what == Traced::merge_started, .start = now});
        break;
    case Traced::built:
    case Traced::build_failed:
    case Traced::merged:
    case Traced::merge_failed:
        running_.erase(step);
        break;
    case Traced::skipped:
        break;
    }
    stale_ = true;
}

std::optional<double> Countdown::left(const Schedule& schedule, double now) {
    if (stale_) {
        std::vector<RunningStep> running;
        running.reserve(running_.size());
        for (const auto& [step, since] : running_) {
            running.push_back(
                {.step = step, .merging = since.merging, .elapsed = now - since.start});
        }
        const auto estimate = estimate_plan(schedule, costs_, cores_, tokens_, running);
        estimated_ = estimate.known == 0 ? std::nullopt : std::optional{estimate.seconds};
        estimated_at_ = now;
        stale_ = false;
    }
    if (!estimated_) {
        return std::nullopt;
    }
    return std::max(*estimated_ - (now - estimated_at_), 0.0);
}

void expect_builds(std::vector<emerge::Snapshot>& snapshots, const BuildKnowledge& knowledge,
                   unsigned cores) {
    for (auto& snapshot : snapshots) {
        for (auto& task : snapshot.tasks) {
            if (task.kind != emerge::TaskKind::build || task.binary ||
                !(task.operation.empty() || task.operation == "merge")) {
                continue;
            }
            if (const auto estimate = knowledge.estimate(task.cpv, cores)) {
                task.expected = estimate->seconds;
            }
        }
    }
}

} // namespace egraph
