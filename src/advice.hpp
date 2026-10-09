#pragma once

// Advice on a plan before it runs, from its estimate and the build history: what its tests cost,
// and the jobs its builds' memory allows.

#include "builds.hpp"
#include "estimate.hpp"
#include "exec.hpp"
#include "steve.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// A build's test phase, as its estimate foretells it.
struct TestCost {
    std::string cpv{};
    double seconds = 0;
    bool operator==(const TestCost&) const = default;
};

// A plan's tests are advised on once they take this long in all, naming the costliest
// test_advice_packages of them.
inline constexpr double test_advice_seconds = 120;
inline constexpr std::size_t test_advice_packages = 3;

// A plan's builds are advised on once the memory they would hold at once passes this share of
// the system's.
inline constexpr double memory_advice_share = 0.8;

struct MemoryAdvice {
    // The most the builds running at once would hold, and the system's memory.
    std::uint64_t peak = 0;
    std::uint64_t total = 0;
    // The most jobs that keep the peak within memory_advice_share, and the plan's seconds and
    // peak at them; none when one at a time does not.
    std::optional<std::uint32_t> jobs{};
    double seconds = 0;
    std::uint64_t jobs_peak = 0;
    // The build that holds the most alone, and how much.
    std::string largest{};
    std::uint64_t largest_peak = 0;
    // Where steve is the jobserver and holds its tokens back at less: the MiB of free memory to
    // hold them back at, the share of the system's beyond memory_advice_share.
    std::optional<std::uint64_t> steve_floor{};
    bool operator==(const MemoryAdvice&) const = default;
};

struct PlanAdvice {
    // The seconds the builds' tests take, summed, and the costliest of them first; none when
    // they take less than test_advice_seconds.
    double test_seconds = 0;
    std::vector<TestCost> tests{};
    std::optional<MemoryAdvice> memory{};
    bool operator==(const PlanAdvice&) const = default;
};

// The advice on a plan's steps from their costs (by step, as step_costs gives them): its tests.
[[nodiscard]] PlanAdvice plan_advice(std::span<const BuildStep> steps,
                                     std::span<const StepCost> costs);

// What the plan (its steps, and built for their packages) would hold at once at jobs (none for
// a token a core) on a system of cores and total bytes of memory, as estimated: none within
// memory_advice_share, else the most jobs below that, found by estimating the plan again at
// fewer, and the build holding the most alone.
[[nodiscard]] std::optional<MemoryAdvice>
memory_advice(const Plan& plan, std::span<const Step> steps, std::span<const BuildStep> built,
              std::span<const StepCost> costs, unsigned cores, std::optional<std::uint32_t> jobs,
              const PlanEstimate& estimate, std::uint64_t total);

// The free MiB steve should hold its tokens back below on a system of total bytes, to keep its
// builds within memory_advice_share; none when its settings hold them back at that or more.
[[nodiscard]] std::optional<std::uint64_t> steve_floor(std::uint64_t total,
                                                       const steve::Settings& settings);

// For scripts, after a plan's estimate line, where advised: "<TAB>advice<TAB>tests<TAB>seconds",
// then "<TAB>advice<TAB>test<TAB>cpv<TAB>seconds" for each package named, seconds whole;
// "<TAB>advice<TAB>memory<TAB>peak<TAB>total<TAB>jobs<TAB>seconds<TAB>jobs peak<TAB>largest
// cpv<TAB>its peak<TAB>steve floor", bytes but the floor's MiB, jobs, its seconds and peak empty
// when there are none, as is the floor.
[[nodiscard]] std::vector<std::string> advice_lines(const PlanAdvice& advice);

// Whether a plan's line is one of advice_lines'.
[[nodiscard]] bool is_advice_line(std::string_view line);

} // namespace egraph
