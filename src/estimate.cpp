#include "estimate.hpp"

#include "human.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>

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

} // namespace egraph
