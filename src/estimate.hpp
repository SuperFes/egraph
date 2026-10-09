#pragma once

// Estimates from the build history: how long a package's next build takes, from its own builds.

#include "builds.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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

} // namespace egraph
