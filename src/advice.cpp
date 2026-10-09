#include "advice.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <utility>

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

std::optional<MemoryAdvice> memory_advice(const Plan& plan, std::span<const Step> steps,
                                          std::span<const BuildStep> built,
                                          std::span<const StepCost> costs, unsigned cores,
                                          std::optional<std::uint32_t> jobs,
                                          const PlanEstimate& estimate, std::uint64_t total) {
    const auto limit = memory_advice_share * static_cast<double>(total);
    if (!estimate.peak_memory || static_cast<double>(*estimate.peak_memory) <= limit) {
        return std::nullopt;
    }
    MemoryAdvice advice{.peak = *estimate.peak_memory, .total = total};
    for (std::size_t i = 0; i < built.size() && i < costs.size(); ++i) {
        const auto& step = built.subspan(i).front();
        const auto peak = costs.subspan(i).front().peak_memory.value_or(0);
        if (!step.uninstall && peak > advice.largest_peak) {
            advice.largest = step.cpv;
            advice.largest_peak = peak;
        }
    }
    const std::vector<Step> all(steps.begin(), steps.end());
    for (auto fewer = jobs.value_or(cores); fewer > 1;) {
        --fewer;
        const auto at = estimate_plan(Schedule{plan, all, fewer}, costs, cores);
        if (static_cast<double>(at.peak_memory.value_or(0)) <= limit) {
            advice.jobs = fewer;
            advice.seconds = at.seconds;
            advice.jobs_peak = at.peak_memory.value_or(0);
            break;
        }
    }
    return advice;
}

std::optional<std::uint64_t> steve_floor(std::uint64_t total, const steve::Settings& settings) {
    constexpr double mib = 1024.0 * 1024.0;
    const auto floor = static_cast<std::uint64_t>(
        std::ceil((1 - memory_advice_share) * static_cast<double>(total) / mib));
    if (settings.min_memory && std::cmp_greater_equal(*settings.min_memory, floor)) {
        return std::nullopt;
    }
    return floor;
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
    if (const auto& memory = advice.memory) {
        const auto optional = [](const auto& value) {
            return value ? std::to_string(*value) : std::string{};
        };
        lines.push_back(
            std::format("\tadvice\tmemory\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", memory->peak,
                        memory->total, optional(memory->jobs),
                        memory->jobs ? std::to_string(std::llround(memory->seconds)) : "",
                        memory->jobs ? std::to_string(memory->jobs_peak) : "", memory->largest,
                        memory->largest_peak, optional(memory->steve_floor)));
    }
    return lines;
}

bool is_advice_line(std::string_view line) {
    return line.starts_with("\tadvice\t");
}

} // namespace egraph
