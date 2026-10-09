#pragma once

// Views of the build history: how long each package takes, the time spent building by week or
// month, what rebuilds most and why, what fails, and what got slower. Each view is a list of plain
// values and the tab-separated lines scripts read; the human layouts draw those lines.

#include "builds.hpp"

#include <array>
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

// A name and how often it came up.
struct Tally {
    std::string name{};
    std::size_t count = 0;
    bool operator==(const Tally&) const = default;
};

// A cp's builds by why they ran.
struct RebuildCounts {
    std::string cp{};
    std::size_t builds = 0;
    // By reason, in BuildReason's order, then those whose reason is not known: a record's own,
    // else inferred from the build of its cp and slot before it (an upgrade, a downgrade, the
    // same version with other flags for USE, else a reinstall).
    std::array<std::size_t, 6> reasons{};
    std::size_t unknown = 0;
    // The flags turned on or off between consecutive builds of a slot, and the cps of the merges
    // its sub-slot rebuilds were for; most first, then by name.
    std::vector<Tally> flags{};
    std::vector<Tally> causes{};
    bool operator==(const RebuildCounts&) const = default;
};

// Each cp built more than once, the most builds first; ties by cp.
[[nodiscard]] std::vector<RebuildCounts> frequent_rebuilds(std::span<const BuildRecord> records);

// For scripts: cp, builds, then the builds for each reason in BuildReason's order and those
// whose reason is not known, then the flags and the causes as name:count separated by spaces.
[[nodiscard]] std::string rebuild_fields(const RebuildCounts& counts);

// A cp's failed builds.
struct Failures {
    std::string cp{};
    std::size_t builds = 0;
    std::size_t failed = 0;
    // The phases that failed ("merge" for the merge, "worker" for a worker's error), most first.
    std::vector<Tally> phases{};
    // Failures that a later build of the same version merged past.
    std::size_t passed_later = 0;
    // The latest failure: when it started, its cpv, its log (or the worker's error), and whether
    // a build of the cp merged after it.
    double last_start = 0;
    std::string last_cpv{};
    std::string last_log{};
    bool merged_since = false;
    bool operator==(const Failures&) const = default;
};

// Each cp with a failed build, the latest failure first.
[[nodiscard]] std::vector<Failures> failing_packages(std::span<const BuildRecord> records);

// For scripts: cp, builds, failed, those that passed later, "merged" or "failing" since, the
// latest failure's start and cpv, the phases as name:count separated by spaces, and the latest
// failure's log or error.
[[nodiscard]] std::string failures_fields(const Failures& failures);

// A latest build this much slower than those before it, and by at least so many seconds.
inline constexpr double slower_share = 1.25;
inline constexpr double slower_seconds = 30;
// The merged builds before the latest it is weighed against, at most.
inline constexpr std::size_t slower_earlier = 5;

// Something that differs between two builds: their version, a toolchain cp's versions, a
// compiler variable, the env files, or the flags (before those turned off, after those turned
// on); empty where it was not there.
struct BuildChange {
    std::string what{};
    std::string before{};
    std::string after{};
    bool operator==(const BuildChange&) const = default;
};

// What differs from before to after, in that order: version, toolchain by cp, environment by
// name, env files, USE.
[[nodiscard]] std::vector<BuildChange> build_changes(const BuildRecord& before,
                                                     const BuildRecord& after);

// A cp whose latest merged build took longer than its earlier ones.
struct Slowdown {
    std::string cp{};
    std::string cpv{};
    double start = 0;
    // Measured in CPU seconds where every build weighed has them, else in seconds building, of
    // builds that ran alone.
    bool cpu = false;
    double seconds = 0;
    // The median of the earlier builds'.
    double before = 0;
    // From the merged build just before it.
    std::vector<BuildChange> changes{};
    bool operator==(const Slowdown&) const = default;
};

// Each cp whose latest merged build took slower_share of the median of the slower_earlier merged
// builds before it, and slower_seconds more; the most slowed first.
[[nodiscard]] std::vector<Slowdown> slower_builds(std::span<const BuildRecord> records);

// For scripts: a line of cp, cpv, start, "cpu" or "wall", seconds and the earlier median, then a
// line per change: an empty field, "change", what, before and after.
[[nodiscard]] std::vector<std::string> slowdown_lines(const Slowdown& slowdown);

} // namespace egraph
