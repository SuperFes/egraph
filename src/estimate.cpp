#include "estimate.hpp"

#include "human.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
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

} // namespace

BuildKnowledge::BuildKnowledge(const std::vector<BuildRecord>& records) {
    for (const auto& record : records) {
        if (record.outcome == BuildRecord::Outcome::merged) {
            auto& found = by_cp_[cp_of(record.cpv)];
            found.push_back(record);
        }
    }
}

std::optional<PackageEstimate> BuildKnowledge::estimate(std::string_view cpv,
                                                        unsigned cores) const {
    const auto found = by_cp_.find(cp_of(cpv));
    if (found == by_cp_.end()) {
        return std::nullopt;
    }
    const auto& all = found->second;
    const bool same_version =
        std::ranges::any_of(all, [&](const BuildRecord& record) { return record.cpv == cpv; });
    std::vector<std::reference_wrapper<const BuildRecord>> chosen;
    for (const auto& record : all | std::views::reverse) {
        if (chosen.size() == estimate_builds) {
            break;
        }
        if (!same_version || record.cpv == cpv) {
            chosen.emplace_back(record);
        }
    }
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
        seconds.push_back(timed ? cpu_seconds / most_parallel : built_in(record));
        if (record.cpu_seconds) {
            cpu.push_back(*record.cpu_seconds);
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

} // namespace egraph
