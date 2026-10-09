#include "advice.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

namespace egraph {

PlanAdvice plan_advice(std::span<const BuildStep> steps, std::span<const StepCost> costs) {
    PlanAdvice advice;
    std::vector<TestCost> tests;
    for (std::size_t i = 0; i < steps.size() && i < costs.size(); ++i) {
        const auto& step = steps.subspan(i).front();
        const auto& cost = costs.subspan(i).front();
        if (!step.uninstall && cost.test_seconds > 0) {
            tests.push_back({.cpv = step.cpv, .seconds = cost.test_seconds});
            advice.test_seconds += cost.test_seconds;
        }
    }
    if (advice.test_seconds < test_advice_seconds) {
        return {};
    }
    std::ranges::stable_sort(tests, std::greater<>{}, &TestCost::seconds);
    tests.resize(std::min(tests.size(), test_advice_packages));
    advice.tests = std::move(tests);
    return advice;
}

std::vector<std::string> advice_lines(const PlanAdvice& advice) {
    std::vector<std::string> lines;
    if (!advice.tests.empty()) {
        lines.push_back(std::format("\tadvice\ttests\t{}", std::llround(advice.test_seconds)));
        for (const auto& test : advice.tests) {
            lines.push_back(
                std::format("\tadvice\ttest\t{}\t{}", test.cpv, std::llround(test.seconds)));
        }
    }
    return lines;
}

bool is_advice_line(std::string_view line) {
    return line.starts_with("\tadvice\t");
}

} // namespace egraph
