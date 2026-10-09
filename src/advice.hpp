#pragma once

// Advice on a plan before it runs, from its estimate and the build history: what its tests cost.

#include "builds.hpp"
#include "estimate.hpp"

#include <cstddef>
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

struct PlanAdvice {
    // The seconds the builds' tests take, summed, and the costliest of them first; none when
    // they take less than test_advice_seconds.
    double test_seconds = 0;
    std::vector<TestCost> tests{};
    bool operator==(const PlanAdvice&) const = default;
};

// The advice on a plan's steps, from their costs (by step, as step_costs gives them).
[[nodiscard]] PlanAdvice plan_advice(std::span<const BuildStep> steps,
                                     std::span<const StepCost> costs);

// For scripts, after a plan's estimate line, where advised: "<TAB>advice<TAB>tests<TAB>seconds",
// then "<TAB>advice<TAB>test<TAB>cpv<TAB>seconds" for each package named, seconds whole.
[[nodiscard]] std::vector<std::string> advice_lines(const PlanAdvice& advice);

// Whether a plan's line is one of advice_lines'.
[[nodiscard]] bool is_advice_line(std::string_view line);

} // namespace egraph
