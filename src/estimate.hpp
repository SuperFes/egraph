#pragma once

// Estimates from the build history: how long a package's next build takes, from its own builds,
// and a plan from its schedule run ahead with those.

#include "builds.hpp"
#include "emerge.hpp"
#include "repository.hpp"

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
    // The builds it rests on, and whether they were of the same version; for a guess, the
    // similar packages it rests on.
    std::size_t builds = 0;
    bool same_version = false;
    // From similar packages, the package never built here.
    bool guessed = false;
    // Of its seconds, those its test phase takes.
    double test_seconds = 0;
    bool operator==(const PackageEstimate&) const = default;
};

// The builds an estimate rests on: the merged ones of the version, else of the cp, those of the
// nearest USE first, then the latest.
inline constexpr std::size_t estimate_builds = 5;

// The most a build's distfiles, against an earlier build's, scale that one's time either way: a
// version's sources rarely grow or shrink more, and a compression change alone moves them some.
inline constexpr double distfile_scale_limit = 2;

// The most similar packages a guess rests on.
inline constexpr std::size_t guess_neighbours = 5;
// The power of the distfiles' size ratio that scales a similar package's time in a guess, until
// the history has guess_fit_packages packages of known sizes to fit it from.
inline constexpr double guess_size_exponent = 0.5;
inline constexpr std::size_t guess_fit_packages = 8;
// The most a guess's distfiles scale a similar package's time either way.
inline constexpr double guess_scale_limit = 10;

// What a build is estimated for: its cpv, and where known the flags it is built with, its
// distfiles' bytes and its eclasses (both lists sorted).
struct BuildWanted {
    std::string cpv{};
    std::optional<std::vector<std::string>> use{};
    std::optional<std::uint64_t> distfiles{};
    std::optional<std::vector<std::string>> eclasses{};
};

// A system's CPU busy share past which a build beside others got less than it asked for.
inline constexpr double saturated_share = 0.9;

// The merged builds of a history by cp, to estimate packages from.
class BuildKnowledge {
  public:
    explicit BuildKnowledge(const std::vector<BuildRecord>& records);

    // The wanted build on a system of cores: none for a cp never built here. A build that ran
    // alone, or beside others on a system not saturated, took what it needed; one on a saturated
    // system is timed from its CPU seconds and the most parallelism the package showed. Each
    // build's time scales by the wanted distfiles' bytes against its own, where both are known.
    [[nodiscard]] std::optional<PackageEstimate> estimate(const BuildWanted& wanted,
                                                          unsigned cores) const;
    [[nodiscard]] std::optional<PackageEstimate> estimate(std::string_view cpv,
                                                          unsigned cores) const {
        return estimate(BuildWanted{.cpv = std::string{cpv}}, cores);
    }

    // Learns, for guesses, each cp's eclasses from index (those of its latest build's version,
    // else of the cp's last version there) and how rare each eclass is among its versions.
    void learn(const RepositoryIndex& index);

    // A build of a package never built here, from the guess_neighbours packages most like it
    // that were: those whose eclasses it shares most, the rarer an eclass the more it weighs,
    // each one's time scaled by the wanted distfiles against its own, to the power the history
    // shows (size_exponent). None without its eclasses, before learn, or with nothing like it.
    [[nodiscard]] std::optional<PackageEstimate> guess(const BuildWanted& wanted,
                                                       unsigned cores) const;

    // The power of a package's distfiles' size its build time grows with, fitted to the latest
    // builds of the packages built here (no less than 0, no more than 1), or
    // guess_size_exponent before enough of them have sizes.
    [[nodiscard]] double size_exponent() const { return size_exponent_; }

    [[nodiscard]] bool empty() const { return by_cp_.empty(); }

  private:
    std::map<std::string, std::vector<BuildRecord>, std::less<>> by_cp_;
    double size_exponent_ = guess_size_exponent;
    // By cp built here, its eclasses, sorted; by eclass, its weight, and the weight of one the
    // index lacks.
    std::map<std::string, std::vector<std::string>, std::less<>> eclasses_;
    std::map<std::string, double, std::less<>> rarity_;
    double unseen_rarity_ = 0;
};

// What a step costs the estimate: a merge step's build and merge, from its package's estimate
// where it has one; an uninstall's run, as merge_seconds.
struct StepCost {
    double build_seconds = 0;
    double parallelism = 1;
    std::optional<std::uint64_t> peak_memory{};
    double merge_seconds = 0;
    // From the package's own builds, or guessed from similar ones.
    bool known = false;
    bool guessed = false;
    // Of build_seconds, those its test phase takes.
    double test_seconds = 0;
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
    // Of the merge steps not done, those whose packages were built here before, those guessed
    // from similar packages, and the rest.
    std::size_t known = 0;
    std::size_t unknown = 0;
    std::size_t guessed = 0;
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

// What the repository index says of a step's package: its distfiles' bytes under the step's USE
// and its eclasses, sorted.
struct StepFacts {
    std::optional<std::uint64_t> distfiles{};
    std::optional<std::vector<std::string>> eclasses{};
    bool operator==(const StepFacts&) const = default;
};

// By step, the facts index holds of its package; nothing for an uninstall or a version it lacks.
[[nodiscard]] std::vector<StepFacts> step_facts(const RepositoryIndex& index,
                                                std::span<const BuildStep> steps);

// Each step's cost on a system of cores: a merge's from its package's estimate where it has
// one, built with the step's USE and with its facts where given, else its guess, else nothing;
// an uninstall's nothing.
[[nodiscard]] std::vector<StepCost> step_costs(const BuildKnowledge& knowledge,
                                               std::span<const BuildStep> steps, unsigned cores,
                                               std::span<const StepFacts> facts = {});

// The jobs a plan is estimated at when none are given: those of the latest build recorded, none
// for no limit.
[[nodiscard]] std::optional<std::uint32_t> recorded_jobs(const std::vector<BuildRecord>& records);

// For scripts, after a plan's lines: "<TAB>estimate<TAB>seconds<TAB>peak<TAB>known<TAB>unknown
// <TAB>jobs<TAB>guessed", the seconds whole, the peak in bytes, empty when unknown, and jobs
// empty for no limit.
[[nodiscard]] std::string estimate_line(const PlanEstimate& estimate,
                                        std::optional<std::uint32_t> jobs);

// A run's time left as it goes: estimated again from its schedule once a step starts or ends,
// and counted down between.
class Countdown {
  public:
    Countdown(std::vector<StepCost> costs, unsigned cores, std::optional<std::uint32_t> tokens);

    void traced(std::size_t step, Traced what, double now);
    // The seconds left at now, from schedule as it stands; none once no step left was built here
    // before or guessed.
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
    std::size_t guessed = 0;
    bool operator==(const EstimateLine&) const = default;
};

// An estimate_line back, or one without its guessed field (as egraph wrote it before guesses);
// none for any other line.
[[nodiscard]] std::optional<EstimateLine> parse_estimate_line(std::string_view line);

} // namespace egraph
