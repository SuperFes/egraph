#pragma once

// Estimates from the build history: how long a package's next build takes, from its own builds,
// and a plan from its schedule run ahead with those.

#include "builds.hpp"
#include "emerge.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// A package's next build as its own last builds foretell it.
struct PackageEstimate {
    // Its build's seconds on a system with nothing else running, fetching left out.
    double seconds = 0;
    // The cores it keeps busy then, on average: the share of the system it competes for beside
    // other builds.
    double parallelism = 1;
    // The most memory any of those builds held.
    std::optional<std::uint64_t> peak_memory{};
    double merge_seconds = 0;
    // The builds it rests on, and whether they were of the same version.
    std::size_t builds = 0;
    bool same_version = false;
    bool operator==(const PackageEstimate&) const = default;
};

// The builds an estimate rests on: the latest merged ones of the version, else of the cp.
inline constexpr std::size_t estimate_builds = 5;

// A system's CPU busy share past which a build beside others got less than it asked for.
inline constexpr double saturated_share = 0.9;

// The merged builds of a history by cp, to estimate packages from.
class BuildKnowledge {
  public:
    explicit BuildKnowledge(const std::vector<BuildRecord>& records);

    // cpv's next build on a system of cores: none for a cp never built here. A build that ran
    // alone, or beside others on a system not saturated, took what it needed; one on a saturated
    // system is timed from its CPU seconds and the most parallelism the package showed.
    [[nodiscard]] std::optional<PackageEstimate> estimate(std::string_view cpv,
                                                          unsigned cores) const;

    [[nodiscard]] bool empty() const { return by_cp_.empty(); }

  private:
    std::map<std::string, std::vector<BuildRecord>, std::less<>> by_cp_;
};

// What a step costs the estimate: a merge step's build and merge, from its package's estimate
// where it has one; an uninstall's run, as merge_seconds.
struct StepCost {
    double build_seconds = 0;
    double parallelism = 1;
    std::optional<std::uint64_t> peak_memory{};
    double merge_seconds = 0;
    // From the package's own builds.
    bool known = false;
    bool operator==(const StepCost&) const = default;
};

// A step running as the estimate starts, building or merging, and its seconds so far.
struct RunningStep {
    std::size_t step = 0;
    bool merging = false;
    double elapsed = 0;
};

struct PlanEstimate {
    // From the start until the last step finishes.
    double seconds = 0;
    // The most memory the builds running at once held, of those whose peaks are known.
    std::optional<std::uint64_t> peak_memory{};
    // Of the merge steps not done, those whose packages were built here before and those not.
    std::size_t known = 0;
    std::size_t unknown = 0;
    // By step, the seconds from the start until it merged or uninstalled; none for one already
    // done or never reached.
    std::vector<std::optional<double>> finished{};
    bool operator==(const PlanEstimate&) const = default;
};

// The plan run ahead on a clock of its own, from schedule as it stands, the steps running
// carried on: each build taking its seconds when the parallelism of those beside it fits the
// cores, and slowed alike when it does not; merges and uninstalls one at a time, as the schedule
// lets them. With tokens, no more builds than that run at once unless one would run alone, as a
// jobserver gates them.
[[nodiscard]] PlanEstimate estimate_plan(Schedule schedule, std::span<const StepCost> costs,
                                         unsigned cores,
                                         std::optional<std::uint32_t> tokens = std::nullopt,
                                         std::span<const RunningStep> running = {});

// Each step's cost on a system of cores: a merge's from its package's estimate where it has
// one, nothing otherwise; an uninstall's nothing.
[[nodiscard]] std::vector<StepCost> step_costs(const BuildKnowledge& knowledge,
                                               std::span<const BuildStep> steps, unsigned cores);

// The jobs a plan is estimated at when none are given: those of the latest build recorded, none
// for no limit.
[[nodiscard]] std::optional<std::uint32_t> recorded_jobs(const std::vector<BuildRecord>& records);

// For scripts, after a plan's lines: "<TAB>estimate<TAB>seconds<TAB>peak<TAB>known<TAB>unknown
// <TAB>jobs", the seconds whole, the peak in bytes, empty when unknown, and jobs empty for no
// limit.
[[nodiscard]] std::string estimate_line(const PlanEstimate& estimate,
                                        std::optional<std::uint32_t> jobs);

// A run's time left as it goes: estimated again from its schedule once a step starts or ends,
// and counted down between.
class Countdown {
  public:
    Countdown(std::vector<StepCost> costs, unsigned cores, std::optional<std::uint32_t> tokens);

    void traced(std::size_t step, Traced what, double now);
    // The seconds left at now, from schedule as it stands; none once no step left was built here
    // before.
    [[nodiscard]] std::optional<double> left(const Schedule& schedule, double now);

  private:
    struct Since {
        bool merging = false;
        double start = 0;
    };
    std::vector<StepCost> costs_;
    unsigned cores_;
    std::optional<std::uint32_t> tokens_;
    std::map<std::size_t, Since> running_;
    std::optional<double> estimated_;
    double estimated_at_ = 0;
    bool stale_ = true;
};

// Sets each build task's expected seconds from what the knowledge foretells of it.
void expect_builds(std::vector<emerge::Snapshot>& snapshots, const BuildKnowledge& knowledge,
                   unsigned cores);

struct EstimateLine {
    double seconds = 0;
    std::optional<std::uint64_t> peak_memory{};
    std::size_t known = 0;
    std::size_t unknown = 0;
    std::optional<std::uint32_t> jobs{};
    bool operator==(const EstimateLine&) const = default;
};

// An estimate_line back; none for any other line.
[[nodiscard]] std::optional<EstimateLine> parse_estimate_line(std::string_view line);

} // namespace egraph
