#include "advice.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <ranges>
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

std::vector<std::string> build_tmpdirs(std::span<const StepCost> costs) {
    std::vector<std::string> tmpdirs;
    for (const auto& cost : costs) {
        if (cost.build_directory) {
            tmpdirs.push_back(cost.build_directory->tmpdir);
        }
    }
    std::ranges::sort(tmpdirs);
    const auto [first, last] = std::ranges::unique(tmpdirs);
    tmpdirs.erase(first, last);
    return tmpdirs;
}

std::vector<TmpfsAdvice>
tmpfs_advice(std::span<const BuildStep> built, std::span<const StepCost> costs,
             const std::map<std::string, os::FilesystemSpace, std::less<>>& spaces) {
    std::vector<TmpfsAdvice> advice;
    for (const auto& [step, cost] : std::views::zip(built, costs)) {
        const auto& directory = cost.build_directory;
        if (step.uninstall || !directory || directory->env_files != step.env_files) {
            continue;
        }
        const auto space = spaces.find(directory->tmpdir);
        if (space != spaces.end() && space->second.tmpfs && directory->bytes > space->second.size) {
            advice.push_back({.cpv = step.cpv,
                              .bytes = directory->bytes,
                              .tmpdir = directory->tmpdir,
                              .size = space->second.size});
        }
    }
    return advice;
}

namespace {

using Flags = std::vector<std::string_view>;

Flags sorted_flags(const RepositoryIndex& index, Range range) {
    Flags found;
    for (const auto id : index.ids_in(range)) {
        found.push_back(index.string(id));
    }
    std::ranges::sort(found);
    return found;
}

Flags sorted_flags(std::span<const std::string> flags) {
    Flags found(flags.begin(), flags.end());
    std::ranges::sort(found);
    return found;
}

Flags enabled(const Flags& use, const Flags& iuse) {
    Flags found;
    std::ranges::set_intersection(use, iuse, std::back_inserter(found));
    return found;
}

Flags differing(const Flags& one, const Flags& other) {
    Flags found;
    std::ranges::set_symmetric_difference(one, other, std::back_inserter(found));
    return found;
}

} // namespace

bool binary_use_matches(const RepositoryIndex& index, const Binary& binary, const BuildStep& step) {
    const auto built_iuse = sorted_flags(index, binary.iuse);
    const auto iuse = sorted_flags(step.iuse);
    Flags added;
    std::ranges::set_difference(differing(built_iuse, iuse), sorted_flags(step.forced),
                                std::back_inserter(added));
    return added.empty() && differing(enabled(sorted_flags(index, binary.use), built_iuse),
                                      enabled(sorted_flags(step.use), iuse))
                                .empty();
}

std::vector<BinaryAdvice> binary_advice(const RepositoryIndex& index,
                                        std::span<const BuildStep> built,
                                        std::span<const StepCost> costs) {
    std::vector<BinaryAdvice> advice;
    const auto cpv_of = [&index](const Binary& binary) { return index.string(binary.cpv); };
    for (const auto& [step, cost] : std::views::zip(built, costs)) {
        if (step.uninstall || cost.build_seconds < binary_advice_seconds) {
            continue;
        }
        const auto same = std::ranges::equal_range(index.binaries, std::string_view{step.cpv},
                                                   std::less<>{}, cpv_of);
        const auto found = std::ranges::find_if(same, [&](const Binary& binary) {
            return binary.ignored == 0 && binary_use_matches(index, binary, step);
        });
        if (found == same.end()) {
            continue;
        }
        const auto& source = index.binary_sources.at(found->source);
        const auto name = index.string(source.name);
        advice.push_back(
            {.cpv = step.cpv,
             .seconds = cost.build_seconds,
             .kind = source.kind,
             .source = std::string{name.empty() ? index.string(source.location) : name},
             .size = found->size});
    }
    return advice;
}

std::optional<OrderAdvice> order_advice(const Plan& plan, std::span<const Step> steps,
                                        std::span<const StepCost> costs, unsigned cores,
                                        std::optional<std::uint32_t> jobs,
                                        const PlanEstimate& estimate) {
    std::optional<OrderAdvice> best;
    for (const auto order : {BuildOrder::critical, BuildOrder::longest}) {
        Schedule schedule{plan, std::vector<Step>(steps.begin(), steps.end()), jobs};
        schedule.prefer(build_order(plan, steps, costs, order));
        const auto at = estimate_plan(std::move(schedule), costs, cores,
                                      jobs ? std::nullopt : std::optional{cores});
        if (!best || at.seconds < best->seconds) {
            best = OrderAdvice{
                .order = order, .seconds = at.seconds, .emerge_seconds = estimate.seconds};
        }
    }
    if (!best || best->seconds > order_advice_share * estimate.seconds ||
        estimate.seconds - best->seconds < order_advice_seconds) {
        return std::nullopt;
    }
    return best;
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
    for (const auto& tmpfs : advice.tmpfs) {
        lines.push_back(std::format("\tadvice\ttmpfs\t{}\t{}\t{}\t{}", tmpfs.cpv, tmpfs.bytes,
                                    tmpfs.tmpdir, tmpfs.size));
    }
    if (const auto& order = advice.order) {
        lines.push_back(std::format("\tadvice\torder\t{}\t{}\t{}", build_order_name(order->order),
                                    std::llround(order->seconds),
                                    std::llround(order->emerge_seconds)));
    }
    for (const auto& binary : advice.binaries) {
        lines.push_back(std::format("\tadvice\tbinary\t{}\t{}\t{}\t{}\t{}", binary.cpv,
                                    std::llround(binary.seconds),
                                    binary.kind == BinarySourceKind::local ? "local" : "binhost",
                                    binary.source, binary.size));
    }
    return lines;
}

bool is_advice_line(std::string_view line) {
    return line.starts_with("\tadvice\t");
}

} // namespace egraph
