#include "keep_going.hpp"

#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using egraph::Standing;
using egraph::test::make_system;

namespace {

std::uint32_t candidate(const egraph::test::System& system, std::string_view cpv) {
    for (std::uint32_t i = 0; i < system.evaluated.candidates.size(); ++i) {
        if (system.evaluated.string(system.evaluated.candidates.at(i).cpv) == cpv) {
            return i;
        }
    }
    FAIL("no candidate " << cpv);
    return 0;
}

std::uint32_t installed(const egraph::test::System& system, std::string_view cpv) {
    for (std::uint32_t id = 0; id < system.store.packages.size(); ++id) {
        if (system.store.string(system.store.packages.at(id).cpv) == cpv) {
            return id;
        }
    }
    FAIL("no installed " << cpv);
    return 0;
}

// A plan merging cpvs in order, each replacing the installed version of its slot, if any.
egraph::Plan plan_of(const egraph::test::System& system, const std::vector<std::string>& cpvs) {
    egraph::Plan plan;
    for (std::uint32_t i = 0; i < cpvs.size(); ++i) {
        egraph::Merge merge;
        merge.candidate = candidate(system, cpvs.at(i));
        const auto& wanted = system.evaluated.candidates.at(merge.candidate);
        for (std::uint32_t id = 0; id < system.store.packages.size(); ++id) {
            const auto& pkg = system.store.packages.at(id);
            if (system.store.string(pkg.cp) == system.evaluated.string(wanted.cp) &&
                system.store.string(pkg.slot) == system.evaluated.string(wanted.slot)) {
                merge.replaces = id;
            }
        }
        plan.merges.push_back(merge);
        plan.order.push_back(i);
    }
    return plan;
}

egraph::Resumption resume(const egraph::test::System& system, const egraph::Plan& plan,
                          const std::vector<Standing>& standing) {
    const auto steps = egraph::run_steps(system.store, system.evaluated, plan, {}, true);
    REQUIRE(steps.size() == standing.size());
    return egraph::keep_going(system.store, system.evaluated, plan, steps, standing);
}

using Skips = std::vector<egraph::Skip>;

} // namespace

TEST_CASE("a merge needing the failed version goes, one an installed version satisfies stays") {
    const auto system =
        make_system({{.cpv = "app-misc/lib-1"}},
                    {{.cpv = "app-misc/lib-2"},
                     {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/lib"}}},
                     {.cpv = "app-misc/user2-1", .deps = {{"RDEPEND", ">=app-misc/lib-2"}}},
                     {.cpv = "app-misc/indep-1"}});
    const auto plan = plan_of(
        system, {"app-misc/lib-2", "app-misc/user-1", "app-misc/user2-1", "app-misc/indep-1"});
    const auto found =
        resume(system, plan, {Standing::gone, Standing::left, Standing::left, Standing::left});
    CHECK(found.skipped == Skips{{.step = 2, .atoms = {">=app-misc/lib-2"}}});
    CHECK_FALSE(found.stuck);
}

TEST_CASE("what reaches a skipped merge goes with it, by the atom it reached it by") {
    const auto system =
        make_system({}, {{.cpv = "app-misc/f-1"},
                         {.cpv = "app-misc/mid-1", .deps = {{"RDEPEND", "app-misc/f"}}},
                         {.cpv = "app-misc/top-1", .deps = {{"DEPEND", "app-misc/mid"}}}});
    const auto plan = plan_of(system, {"app-misc/f-1", "app-misc/mid-1", "app-misc/top-1"});
    const auto found = resume(system, plan, {Standing::gone, Standing::left, Standing::left});
    CHECK(found.skipped ==
          Skips{{.step = 1, .atoms = {"app-misc/f"}}, {.step = 2, .atoms = {"app-misc/mid"}}});
    CHECK_FALSE(found.stuck);
}

TEST_CASE("what reaches a skipped merge stays while something installed matches its atom") {
    const auto system =
        make_system({{.cpv = "app-misc/mid-1"}},
                    {{.cpv = "app-misc/f-1"},
                     {.cpv = "app-misc/mid-2", .deps = {{"RDEPEND", ">=app-misc/f-1"}}},
                     {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/mid"}}}});
    const auto plan = plan_of(system, {"app-misc/f-1", "app-misc/mid-2", "app-misc/top-1"});
    const auto found = resume(system, plan, {Standing::gone, Standing::left, Standing::left});
    CHECK(found.skipped == Skips{{.step = 1, .atoms = {">=app-misc/f-1"}}});
}

TEST_CASE("what the steps done merged is installed, and what they replaced is not") {
    const auto system =
        make_system({{.cpv = "app-misc/lib-1"}},
                    {{.cpv = "app-misc/lib-2"},
                     {.cpv = "app-misc/x-1"},
                     {.cpv = "app-misc/a-1", .deps = {{"RDEPEND", ">=app-misc/lib-2"}}},
                     {.cpv = "app-misc/d-1", .deps = {{"RDEPEND", "<app-misc/lib-2"}}}});
    const auto plan =
        plan_of(system, {"app-misc/lib-2", "app-misc/x-1", "app-misc/a-1", "app-misc/d-1"});
    const auto found =
        resume(system, plan, {Standing::done, Standing::gone, Standing::left, Standing::left});
    CHECK(found.skipped == Skips{{.step = 3, .atoms = {"<app-misc/lib-2"}}});
}

TEST_CASE("a merge's build-time dependencies count, an installed package's do not") {
    const auto system =
        make_system({{.cpv = "app-misc/n-1", .deps = {{"DEPEND", "app-misc/gone"}}}},
                    {{.cpv = "app-misc/f-1"},
                     {.cpv = "app-misc/b-1", .deps = {{"BDEPEND", "app-misc/f"}}},
                     {.cpv = "app-misc/c-1", .deps = {{"RDEPEND", "app-misc/n"}}}});
    const auto plan = plan_of(system, {"app-misc/f-1", "app-misc/b-1", "app-misc/c-1"});
    const auto found = resume(system, plan, {Standing::gone, Standing::left, Standing::left});
    CHECK(found.skipped == Skips{{.step = 1, .atoms = {"app-misc/f"}}});
    CHECK_FALSE(found.stuck);
}

TEST_CASE(
    "an installed package a merge reaches, with a run-time dependency unsatisfied, is stuck") {
    const auto system =
        make_system({{.cpv = "app-misc/n-1", .deps = {{"RDEPEND", "app-misc/gone"}}}},
                    {{.cpv = "app-misc/f-1"},
                     {.cpv = "app-misc/g-1", .deps = {{"RDEPEND", "app-misc/f"}}},
                     {.cpv = "app-misc/d-1", .deps = {{"RDEPEND", "app-misc/n"}}},
                     {.cpv = "app-misc/c-1", .deps = {{"RDEPEND", "app-misc/d"}}}});
    const auto plan =
        plan_of(system, {"app-misc/f-1", "app-misc/g-1", "app-misc/d-1", "app-misc/c-1"});
    // Through a merge left, then the installed package; after g goes, nothing more can.
    const auto found =
        resume(system, plan, {Standing::gone, Standing::left, Standing::left, Standing::left});
    CHECK(found.stuck == "app-misc/n-1: app-misc/gone");
    CHECK(found.skipped.empty());
    // Once d is merged, no merge left reaches n.
    const auto merged =
        resume(system, plan, {Standing::gone, Standing::left, Standing::done, Standing::done});
    CHECK_FALSE(merged.stuck);
    CHECK(merged.skipped == Skips{{.step = 1, .atoms = {"app-misc/f"}}});
}

TEST_CASE("an installed package a merge left replaces is not reached") {
    const auto system =
        make_system({{.cpv = "app-misc/n-1", .deps = {{"RDEPEND", "app-misc/gone"}}}},
                    {{.cpv = "app-misc/f-1"},
                     {.cpv = "app-misc/n-2"},
                     {.cpv = "app-misc/c-1", .deps = {{"RDEPEND", "app-misc/n"}}}});
    const auto plan = plan_of(system, {"app-misc/f-1", "app-misc/n-2", "app-misc/c-1"});
    const auto found = resume(system, plan, {Standing::gone, Standing::left, Standing::left});
    CHECK_FALSE(found.stuck);
    CHECK(found.skipped.empty());
}

TEST_CASE("a || is satisfied by any of its members") {
    const auto system = make_system(
        {{.cpv = "app-misc/other-1"}},
        {{.cpv = "app-misc/f-1"},
         {.cpv = "app-misc/either-1", .deps = {{"RDEPEND", "|| ( app-misc/f app-misc/other )"}}},
         {.cpv = "app-misc/only-1", .deps = {{"RDEPEND", "|| ( app-misc/f app-misc/missing )"}}}});
    const auto plan = plan_of(system, {"app-misc/f-1", "app-misc/either-1", "app-misc/only-1"});
    const auto found = resume(system, plan, {Standing::gone, Standing::left, Standing::left});
    CHECK(found.skipped == Skips{{.step = 2, .atoms = {"app-misc/f", "app-misc/missing"}}});
}

TEST_CASE("an uninstall goes once none of the merges it waits for is done or left") {
    const auto system =
        make_system({{.cpv = "app-misc/old-1"}},
                    {{.cpv = "app-misc/new-1", .deps = {{"RDEPEND", "!app-misc/old"}}},
                     {.cpv = "app-misc/also-1", .deps = {{"RDEPEND", "!app-misc/old"}}}});
    auto plan = plan_of(system, {"app-misc/new-1", "app-misc/also-1"});
    plan.uninstalls = {
        {.package = installed(system, "app-misc/old-1"), .why = egraph::Block{}, .after = {0, 1}}};
    CHECK(resume(system, plan, {Standing::gone, Standing::gone, Standing::left}).skipped ==
          Skips{{.step = 2, .atoms = {}}});
    CHECK(resume(system, plan, {Standing::gone, Standing::left, Standing::left}).skipped.empty());
    CHECK(resume(system, plan, {Standing::gone, Standing::done, Standing::left}).skipped.empty());
}

TEST_CASE("a skip says what it needs") {
    CHECK(egraph::describe_skip({.step = 0, .atoms = {"app-misc/f", "app-misc/g"}}) ==
          "needs app-misc/f, app-misc/g");
    CHECK(egraph::describe_skip({.step = 0, .atoms = {}}) == "no merge left needs it gone");
}
