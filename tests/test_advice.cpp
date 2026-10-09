#include "advice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::BuildStep;
using egraph::PlanAdvice;
using egraph::StepCost;
using egraph::TestCost;

namespace {

StepCost tested(double seconds, double test) {
    return {.build_seconds = seconds, .known = true, .test_seconds = test};
}

} // namespace

TEST_CASE("a plan's tests are advised on once they cost enough, the costliest named") {
    const std::vector<BuildStep> steps{
        {.cpv = "app-misc/a-1"}, {.cpv = "app-misc/b-1"},
        {.cpv = "app-misc/c-1"}, {.cpv = "app-misc/d-1"},
        {.cpv = "app-misc/e-1"}, {.cpv = "app-misc/a-0", .uninstall = true}};
    const std::vector<StepCost> costs{tested(100, 10), tested(500, 90), {},
                                      tested(50, 0),   tested(300, 40), tested(5, 3)};
    CHECK(egraph::plan_advice(steps, costs) ==
          PlanAdvice{.test_seconds = 140,
                     .tests = {{.cpv = "app-misc/b-1", .seconds = 90},
                               {.cpv = "app-misc/e-1", .seconds = 40},
                               {.cpv = "app-misc/a-1", .seconds = 10}}});
    // Too little to say.
    const std::vector<StepCost> cheap{tested(100, 10), tested(500, 90), {}, {}, {}, {}};
    CHECK(egraph::plan_advice(steps, cheap) == PlanAdvice{});
    CHECK(egraph::plan_advice({}, {}) == PlanAdvice{});
}

TEST_CASE("a plan's advice is lines for scripts") {
    const PlanAdvice advice{.test_seconds = 140.4,
                            .tests = {{.cpv = "app-misc/b-1", .seconds = 90.6},
                                      {.cpv = "app-misc/e-1", .seconds = 40}}};
    const auto lines = egraph::advice_lines(advice);
    CHECK(lines == std::vector<std::string>{"\tadvice\ttests\t140",
                                            "\tadvice\ttest\tapp-misc/b-1\t91",
                                            "\tadvice\ttest\tapp-misc/e-1\t40"});
    for (const auto& line : lines) {
        CHECK(egraph::is_advice_line(line));
    }
    CHECK(egraph::advice_lines(PlanAdvice{}).empty());
    CHECK_FALSE(egraph::is_advice_line("\testimate\t10\t\t1\t0\t\t0"));
    CHECK_FALSE(egraph::is_advice_line("app-misc/a\t1\t2"));
}
