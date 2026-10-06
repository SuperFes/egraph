#include "run_state.hpp"

#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::RunState;
using egraph::test::make_system;

TEST_CASE("the last run's record is kept under the root's /var/lib/egraph") {
    CHECK(egraph::run_state_path("/mnt/gentoo") == "/mnt/gentoo/var/lib/egraph/exec.json");
}

TEST_CASE("a run's record reads back as written") {
    const RunState state{.arguments = {"-u", "--jobs", "4", "@world"},
                         .merged = {"app-misc/a-1", "app-misc/b-2"},
                         .failed = {{.cpv = "app-misc/c-1", .log = "/var/tmp/c.log"}},
                         .status = RunState::Status::failed};
    const auto read = egraph::parse_run_state(egraph::run_state_json(state));
    REQUIRE(read);
    CHECK(read->arguments == state.arguments);
    CHECK(read->merged == state.merged);
    CHECK(read->failed == state.failed);
    CHECK(read->status == RunState::Status::failed);
    for (const auto status : {RunState::Status::running, RunState::Status::done}) {
        CHECK(egraph::parse_run_state(egraph::run_state_json({.status = status}))->status ==
              status);
    }
}

TEST_CASE("a record from before failures were kept has none") {
    const auto read =
        egraph::parse_run_state(R"({"arguments": [], "merged": [], "status": "failed"})");
    REQUIRE(read);
    CHECK(read->failed.empty());
}

TEST_CASE("a record that is not one is an error") {
    CHECK_FALSE(egraph::parse_run_state(""));
    CHECK_FALSE(egraph::parse_run_state("[]"));
    CHECK_FALSE(egraph::parse_run_state(R"({"arguments": [], "merged": []})"));
    CHECK_FALSE(
        egraph::parse_run_state(R"({"arguments": [1], "merged": [], "status": "running"})"));
    CHECK_FALSE(egraph::parse_run_state(R"({"arguments": [], "merged": [], "status": "halfway"})"));
    CHECK_FALSE(egraph::parse_run_state(
        R"({"arguments": [], "merged": [], "failed": [{"cpv": "a/b-1"}], "status": "failed"})"));
    CHECK_FALSE(egraph::parse_run_state(
        R"({"arguments": [], "merged": [], "failed": {}, "status": "failed"})"));
}

TEST_CASE("a resumed run leaves out the merges of what it merged and is installed still") {
    // foo-1 reinstalled, bar updated, baz new.
    const auto system = make_system(
        {{.cpv = "app-misc/foo-1"}, {.cpv = "app-misc/bar-1"}},
        {{.cpv = "app-misc/foo-1"}, {.cpv = "app-misc/bar-2"}, {.cpv = "app-misc/baz-1"}});
    const auto candidate = [&](std::string_view cpv) {
        for (std::uint32_t i = 0; i < system.evaluated.candidates.size(); ++i) {
            if (system.evaluated.string(system.evaluated.candidates.at(i).cpv) == cpv) {
                return i;
            }
        }
        FAIL("no candidate " << cpv);
        return 0U;
    };
    egraph::Plan plan;
    plan.merges.resize(3);
    plan.merges.at(0).candidate = candidate("app-misc/foo-1");
    plan.merges.at(1).candidate = candidate("app-misc/bar-2");
    plan.merges.at(2).candidate = candidate("app-misc/baz-1");
    std::vector<egraph::Step> steps;
    for (std::uint32_t merge = 0; merge < 3; ++merge) {
        steps.emplace_back(egraph::MergeStep{.merge = merge, .blockers = {}, .world = {}});
    }
    steps.emplace_back(egraph::UninstallStep{.uninstall = 0, .clean_world = false});
    // baz-1 merged and gone since; bar-1 merged, but the plan moves it on.
    const std::vector<std::string> merged{"app-misc/foo-1", "app-misc/baz-1", "app-misc/bar-1"};
    const auto left = egraph::resumed_steps(system.store, system.evaluated, plan, steps, merged);
    REQUIRE(left.size() == 3);
    CHECK(std::get<egraph::MergeStep>(left.at(0)).merge == 1);
    CHECK(std::get<egraph::MergeStep>(left.at(1)).merge == 2);
    CHECK(std::holds_alternative<egraph::UninstallStep>(left.at(2)));
    CHECK(egraph::resumed_steps(system.store, system.evaluated, plan, steps, {}).size() == 4);
}
