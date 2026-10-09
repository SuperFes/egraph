#pragma once

// Views of the build history: how long each package takes, and the time spent building by week
// or month. Each view is a list of plain values and the tab-separated lines scripts read; the
// human layouts draw those lines.

#include "builds.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// A cp's builds, summed up.
struct PackageTimes {
    std::string cp{};
    // Its merged builds' seconds without fetching, oldest first, and its failed builds.
    std::vector<double> seconds{};
    std::size_t failed = 0;
    double median = 0;
    // The median of those whose CPU time was measured, and the most memory any held.
    std::optional<double> cpu{};
    std::optional<std::uint64_t> peak_memory{};
    bool operator==(const PackageTimes&) const = default;
};

// Each cp with a merged build, the slowest first by its median; ties by cp.
[[nodiscard]] std::vector<PackageTimes> slowest_packages(std::span<const BuildRecord> records);

// For scripts: cp, merged builds, failed builds, median seconds, median CPU seconds and peak
// memory (empty where not measured), and the merged builds' seconds, oldest first, separated by
// spaces.
[[nodiscard]] std::string package_times_fields(const PackageTimes& times);

enum class BuildPeriod : std::uint8_t { week, month };

// The builds that started in a week (from Monday) or month, local time.
struct PeriodTotal {
    // The period's first day.
    std::chrono::year_month_day start{};
    std::size_t builds = 0;
    std::size_t failed = 0;
    // Building and merging, failed builds included.
    double seconds = 0;
    // Of the builds whose CPU time was measured.
    std::optional<double> cpu{};
    bool operator==(const PeriodTotal&) const = default;
};

// Every period from the first build's to the last's, those without builds included.
[[nodiscard]] std::vector<PeriodTotal> period_totals(std::span<const BuildRecord> records,
                                                     BuildPeriod period,
                                                     const std::chrono::time_zone& zone);

// For scripts: the first day (2026-10-05), builds, failed, seconds, and CPU seconds (empty where
// none was measured).
[[nodiscard]] std::string period_fields(const PeriodTotal& total);

} // namespace egraph
