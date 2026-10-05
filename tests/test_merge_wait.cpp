#include "merge_wait.hpp"

#include "graph.hpp"
#include "plan.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using egraph::MergeWaitScope;
using egraph::test::make_system;

namespace {

// The cpvs of the plan's merges that merge alone under scope.
std::set<std::string> alone(const egraph::test::System& system, MergeWaitScope scope) {
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    std::vector<egraph::Step> steps;
    for (const auto merge : plan.order) {
        steps.emplace_back(egraph::MergeStep{.merge = merge, .blockers = {}, .world = {}});
    }
    const auto graph = egraph::build_graph(system.store);
    const auto held =
        egraph::merge_wait_steps(system.store, graph, system.evaluated, plan, steps, scope);
    const auto& evaluated = plan.evaluated_or(system.evaluated);
    std::set<std::string> found;
    for (std::size_t step = 0; step < steps.size(); ++step) {
        if (held.at(step)) {
            const auto merge = std::get<egraph::MergeStep>(steps.at(step)).merge;
            found.emplace(
                evaluated.string(evaluated.candidates.at(plan.merges.at(merge).candidate).cpv));
        }
    }
    return found;
}

// glibc and gcc in @system and updating; glibc's new version needs a new libx at run time and
// gcc's a new libb to build; bash in @system and staying, needing readline, which updates; foo
// updates on its own.
egraph::test::System updating() {
    return make_system({{.cpv = "sys-libs/glibc-1"},
                        {.cpv = "sys-devel/gcc-1"},
                        {.cpv = "dev-libs/libx-1"},
                        {.cpv = "dev-libs/libb-1"},
                        {.cpv = "app-shells/bash-5", .deps = {{"RDEPEND", "sys-libs/readline"}}},
                        {.cpv = "sys-libs/readline-1"},
                        {.cpv = "app-misc/foo-1"}},
                       {{.cpv = "sys-libs/glibc-2", .deps = {{"RDEPEND", ">=dev-libs/libx-2"}}},
                        {.cpv = "sys-devel/gcc-2", .deps = {{"DEPEND", ">=dev-libs/libb-2"}}},
                        {.cpv = "dev-libs/libx-2"},
                        {.cpv = "dev-libs/libb-2"},
                        {.cpv = "app-shells/bash-5", .deps = {{"RDEPEND", "sys-libs/readline"}}},
                        {.cpv = "sys-libs/readline-2"},
                        {.cpv = "app-misc/foo-2"}},
                       {}, {"sys-libs/glibc", "sys-devel/gcc", "app-shells/bash"});
}

} // namespace

TEST_CASE("the system scope holds the merges @system's atoms match") {
    CHECK(alone(updating(), MergeWaitScope::system) ==
          std::set<std::string>{"sys-libs/glibc-2", "sys-devel/gcc-2"});
}

TEST_CASE("the deep scope adds what they and the @system members staying need at run time") {
    // Not libb, which gcc only builds with; nor foo.
    CHECK(alone(updating(), MergeWaitScope::deep) ==
          std::set<std::string>{"sys-libs/glibc-2", "sys-devel/gcc-2", "dev-libs/libx-2",
                                "sys-libs/readline-2"});
}

TEST_CASE("the toolchain scope holds its fixed list, @system or not") {
    auto system = make_system({{.cpv = "sys-devel/gcc-1"}, {.cpv = "app-misc/core-1"}},
                              {{.cpv = "sys-devel/gcc-2"}, {.cpv = "app-misc/core-2"}}, {},
                              {"app-misc/core"});
    CHECK(alone(system, MergeWaitScope::toolchain) == std::set<std::string>{"sys-devel/gcc-2"});
    CHECK(alone(system, MergeWaitScope::none).empty());
}

TEST_CASE("an uninstall never merges alone") {
    const auto system = updating();
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    const std::vector<egraph::Step> steps{
        egraph::UninstallStep{.uninstall = 0, .clean_world = false}};
    const auto graph = egraph::build_graph(system.store);
    CHECK(egraph::merge_wait_steps(system.store, graph, system.evaluated, plan, steps,
                                   MergeWaitScope::deep) == std::vector<bool>{false});
}

TEST_CASE("a merge's run-time waits are its RDEPEND and PDEPEND ones") {
    egraph::Plan plan;
    plan.merges.resize(4);
    plan.merges.at(0).waits = {{.merge = 1, .kinds = {.run = true}},
                               {.merge = 2, .kinds = {.build = true}},
                               {.merge = 3, .kinds = {.post = true}}};
    CHECK(egraph::run_time_waits(plan, 0) == std::vector<std::uint32_t>{1, 3});
}

TEST_CASE("--merge-wait-scope names a scope") {
    CHECK(egraph::merge_wait_scope("deep") == MergeWaitScope::deep);
    CHECK(egraph::merge_wait_scope("toolchain") == MergeWaitScope::toolchain);
    CHECK(egraph::merge_wait_scope("none") == MergeWaitScope::none);
    CHECK_FALSE(egraph::merge_wait_scope("all"));
}
