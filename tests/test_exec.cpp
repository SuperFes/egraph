#include "exec.hpp"

#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

using egraph::test::make_system;
using Json = nlohmann::json;

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

std::optional<std::string> world(const egraph::test::System& system, std::string_view cpv,
                                 const std::vector<egraph::Argument>& arguments) {
    return egraph::world_atom(system.store, system.evaluated,
                              system.evaluated.candidates.at(candidate(system, cpv)), arguments);
}

std::vector<egraph::Argument> atoms(std::initializer_list<std::string> texts) {
    std::vector<egraph::Argument> found;
    for (const auto& text : texts) {
        found.push_back({.set = "", .atom = text});
    }
    return found;
}

egraph::Merge merge(std::uint32_t candidate, std::optional<std::uint32_t> replaces = std::nullopt) {
    egraph::Merge found;
    found.candidate = candidate;
    found.replaces = replaces;
    return found;
}

// The steps as their requests.
std::vector<Json> requests(const egraph::test::System& system, const egraph::Plan& plan,
                           const std::vector<egraph::Argument>& arguments, bool oneshot) {
    std::vector<Json> found;
    for (const auto& step :
         egraph::run_steps(system.store, system.evaluated, plan, arguments, oneshot)) {
        found.push_back(
            Json::parse(egraph::worker_request(system.store, system.evaluated, plan, step)));
    }
    return found;
}

} // namespace

TEST_CASE("a merge records its argument's cp, unless nothing but a set names it") {
    const auto system = make_system({}, {{.cpv = "app-misc/foo-1"}, {.cpv = "app-misc/bar-1"}});
    CHECK(world(system, "app-misc/foo-1", atoms({"app-misc/foo"})) == "app-misc/foo");
    CHECK(world(system, "app-misc/foo-1", atoms({"=app-misc/foo-1"})) == "app-misc/foo");
    CHECK(world(system, "app-misc/foo-1", atoms({"app-misc/bar"})) == std::nullopt);
    CHECK(world(system, "app-misc/foo-1", {{.set = "world", .atom = "app-misc/foo"}}) ==
          std::nullopt);
}

TEST_CASE("an argument naming one slot of a slotted cp records the slot atom") {
    const auto system = make_system({}, {{.cpv = "app-misc/foo-1", .slot = "1"},
                                         {.cpv = "app-misc/foo-2", .slot = "2"},
                                         {.cpv = "app-misc/foo-2.1", .slot = "2"}});
    CHECK(world(system, "app-misc/foo-2", atoms({"app-misc/foo:2"})) == "app-misc/foo:2");
    CHECK(world(system, "app-misc/foo-2", atoms({"=app-misc/foo-2"})) == "app-misc/foo:2");
    CHECK(world(system, "app-misc/foo-2", atoms({">=app-misc/foo-2"})) == "app-misc/foo:2");
    // The cp alone, or an atom both slots match, is the cp.
    CHECK(world(system, "app-misc/foo-2", atoms({"app-misc/foo"})) == "app-misc/foo");
    CHECK(world(system, "app-misc/foo-2", atoms({">=app-misc/foo-1"})) == "app-misc/foo");
    // The most specific argument decides.
    CHECK(world(system, "app-misc/foo-2", atoms({"app-misc/foo", "app-misc/foo:2"})) ==
          "app-misc/foo:2");
}

TEST_CASE("an unslotted cp in a slot other than 0 counts as slotted") {
    const auto system = make_system({}, {{.cpv = "app-misc/foo-1", .slot = "3"}});
    CHECK(world(system, "app-misc/foo-1", atoms({"=app-misc/foo-1"})) == "app-misc/foo:3");
}

TEST_CASE("installed slots make a cp slotted when the repositories have one") {
    const auto system = make_system(
        {{.cpv = "app-misc/foo-1", .slot = "1"}, {.cpv = "app-misc/foo-2", .slot = "2"}},
        {{.cpv = "app-misc/foo-3"}});
    CHECK(world(system, "app-misc/foo-3", atoms({"=app-misc/foo-3"})) == "app-misc/foo:0");
}

TEST_CASE("a merge the world file selects already records nothing") {
    const auto system = make_system(
        {}, {{.cpv = "app-misc/foo-1", .slot = "1"}, {.cpv = "app-misc/foo-2", .slot = "2"}},
        {"app-misc/foo:2"});
    CHECK(world(system, "app-misc/foo-2", atoms({"app-misc/foo:2"})) == std::nullopt);
    CHECK(world(system, "app-misc/foo-2", atoms({"app-misc/foo"})) == "app-misc/foo");
}

TEST_CASE("an unslotted system package records nothing, but for a virtual") {
    const auto system = make_system({}, {{.cpv = "app-misc/foo-1"}, {.cpv = "virtual/foo-1"}}, {},
                                    {"app-misc/foo", "virtual/foo"});
    CHECK(world(system, "app-misc/foo-1", atoms({"app-misc/foo"})) == std::nullopt);
    CHECK(world(system, "virtual/foo-1", atoms({"virtual/foo"})) == "virtual/foo");
}

TEST_CASE("an argument's repository is recorded with it") {
    const auto system = make_system({}, {{.cpv = "app-misc/foo-1", .repo = "other"},
                                         {.cpv = "app-misc/foo-1", .repo = "test_repo"}});
    const auto& evaluated = system.evaluated;
    for (const auto& found : evaluated.candidates) {
        if (evaluated.string(found.repo) == "other") {
            CHECK(egraph::world_atom(system.store, evaluated, found,
                                     atoms({"app-misc/foo::other"})) == "app-misc/foo::other");
        }
    }
}

TEST_CASE("a merge takes its run-time blockers either way, but in its own slot") {
    auto system = make_system({{.cpv = "app-misc/old-1"},
                               {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "!app-misc/new"}}},
                               {.cpv = "app-misc/builds-1", .deps = {{"DEPEND", "!app-misc/new"}}},
                               {.cpv = "app-misc/new-1"}},
                              {{.cpv = "app-misc/new-2",
                                .deps = {{"RDEPEND", "!app-misc/old !<app-misc/new-2"},
                                         {"DEPEND", "!app-misc/builds"}}}});
    egraph::Plan plan;
    plan.merges.push_back(
        merge(candidate(system, "app-misc/new-2"), installed(system, "app-misc/new-1")));
    plan.order = {0};
    const auto found = requests(system, plan, {}, true);
    REQUIRE(found.size() == 1);
    CHECK(found.at(0) == Json::parse(R"({"cpv": "app-misc/new-2", "repo": "test_repo",
                                         "blockers": ["app-misc/old-1", "app-misc/holder-1"]})"));
}

TEST_CASE("each uninstall follows the merges it waits for, gone for the merges after it") {
    auto system = make_system({{.cpv = "app-misc/old-1"}, {.cpv = "app-misc/lib-1"}},
                              {{.cpv = "app-misc/new-1", .deps = {{"RDEPEND", "!app-misc/old"}}},
                               {.cpv = "app-misc/also-1", .deps = {{"RDEPEND", "!app-misc/old"}}},
                               {.cpv = "app-misc/lib-2", .deps = {{"RDEPEND", "!app-misc/old"}}}});
    egraph::Plan plan;
    plan.merges = {merge(candidate(system, "app-misc/new-1")),
                   merge(candidate(system, "app-misc/also-1")),
                   merge(candidate(system, "app-misc/lib-2"), installed(system, "app-misc/lib-1"))};
    plan.order = {1, 0, 2};
    plan.uninstalls = {
        {.package = installed(system, "app-misc/old-1"), .why = egraph::Block{}, .after = {0, 1}}};
    const auto found = requests(system, plan, atoms({"app-misc/new", "app-misc/old"}), false);
    CHECK(found ==
          std::vector<Json>{Json::parse(R"({"cpv": "app-misc/also-1", "repo": "test_repo",
                                       "blockers": ["app-misc/old-1"]})"),
                            Json::parse(R"({"cpv": "app-misc/new-1", "repo": "test_repo",
                                       "blockers": ["app-misc/old-1"], "world": "app-misc/new"})"),
                            Json::parse(R"({"uninstall": "app-misc/old-1", "clean_world": true})"),
                            Json::parse(R"({"cpv": "app-misc/lib-2", "repo": "test_repo"})")});
    const auto steps = egraph::run_steps(system.store, system.evaluated, plan, {}, true);
    CHECK(egraph::step_cpv(system.store, system.evaluated, plan, steps.at(2)) == "app-misc/old-1");
    CHECK(egraph::step_cpv(system.store, system.evaluated, plan, steps.at(3)) == "app-misc/lib-2");
    // Under --oneshot, the world file is left alone.
    const auto oneshot = requests(system, plan, atoms({"app-misc/new", "app-misc/old"}), true);
    CHECK_FALSE(oneshot.at(1).contains("world"));
    CHECK(oneshot.at(2) == Json::parse(R"({"uninstall": "app-misc/old-1"})"));
}

TEST_CASE("a build and its merge are requested apart, the merge's blockers as the run stands") {
    auto system = make_system({{.cpv = "app-misc/old-1"}, {.cpv = "app-misc/lib-1"}},
                              {{.cpv = "app-misc/new-1", .deps = {{"RDEPEND", "!app-misc/old"}}},
                               {.cpv = "app-misc/also-1", .deps = {{"RDEPEND", "!app-misc/old"}}},
                               {.cpv = "app-misc/lib-2", .deps = {{"RDEPEND", "!app-misc/old"}}}});
    egraph::Plan plan;
    plan.merges = {merge(candidate(system, "app-misc/new-1")),
                   merge(candidate(system, "app-misc/also-1")),
                   merge(candidate(system, "app-misc/lib-2"), installed(system, "app-misc/lib-1"))};
    plan.order = {1, 0, 2};
    plan.uninstalls = {
        {.package = installed(system, "app-misc/old-1"), .why = egraph::Block{}, .after = {0, 1}}};
    const auto arguments = atoms({"app-misc/new", "app-misc/old"});
    egraph::StepRequests requests{
        system.store, system.evaluated, plan,
        egraph::run_steps(system.store, system.evaluated, plan, arguments, false)};
    CHECK(Json::parse(requests.build(0)) ==
          Json::parse(R"({"build": "app-misc/also-1", "repo": "test_repo"})"));
    CHECK(Json::parse(requests.merge(1)) ==
          Json::parse(R"({"merge": "app-misc/new-1", "blockers": ["app-misc/old-1"],
                          "world": "app-misc/new"})"));
    CHECK(Json::parse(requests.merge(2)) ==
          Json::parse(R"({"uninstall": "app-misc/old-1", "clean_world": true})"));
    // Merging before old-1 goes, unlike in the steps' order, lib-2 takes it as a blocker.
    CHECK(Json::parse(requests.merge(3)) ==
          Json::parse(R"({"merge": "app-misc/lib-2", "blockers": ["app-misc/old-1"]})"));
    requests.done(1);
    requests.done(0);
    requests.done(2);
    CHECK(Json::parse(requests.merge(3)) == Json::parse(R"({"merge": "app-misc/lib-2"})"));
}

TEST_CASE("what a merge replaces no longer blocks the merges after it") {
    auto system = make_system({{.cpv = "app-misc/old-1", .deps = {{"RDEPEND", "!app-misc/b"}}}},
                              {{.cpv = "app-misc/old-2"}, {.cpv = "app-misc/b-1"}});
    egraph::Plan plan;
    plan.merges = {merge(candidate(system, "app-misc/old-2"), installed(system, "app-misc/old-1")),
                   merge(candidate(system, "app-misc/b-1"))};
    plan.order = {1, 0};
    CHECK(requests(system, plan, {}, true).at(0).contains("blockers"));
    plan.order = {0, 1};
    CHECK_FALSE(requests(system, plan, {}, true).at(1).contains("blockers"));
}

TEST_CASE("the worker's events are read from its lines") {
    using Kind = egraph::WorkerEvent::Kind;
    CHECK(egraph::parse_event(R"({"phase": "compile"})") ==
          egraph::WorkerEvent{.kind = Kind::phase, .text = "compile", .status = 0, .log = ""});
    CHECK(egraph::parse_event(R"({"built": "a/b-1"})") ==
          egraph::WorkerEvent{.kind = Kind::built, .text = "a/b-1", .status = 0, .log = ""});
    CHECK(egraph::parse_event(R"({"merged": "a/b-1"})") ==
          egraph::WorkerEvent{.kind = Kind::merged, .text = "a/b-1", .status = 0, .log = ""});
    CHECK(egraph::parse_event(R"({"uninstalled": "a/b-1"})") ==
          egraph::WorkerEvent{.kind = Kind::uninstalled, .text = "a/b-1", .status = 0, .log = ""});
    CHECK(egraph::parse_event(R"({"failed": "compile", "status": 1, "log": "/l"})") ==
          egraph::WorkerEvent{.kind = Kind::failed, .text = "compile", .status = 1, .log = "/l"});
    CHECK(egraph::parse_event(R"({"error": "no ebuild"})") ==
          egraph::WorkerEvent{.kind = Kind::error, .text = "no ebuild", .status = 0, .log = ""});
    for (const auto* line :
         {"not json", "[]", "{}", R"({"phase": 1})", R"({"failed": "x"})",
          R"({"failed": "x", "status": "1", "log": "/l"})", R"({"other": "x"})"}) {
        CAPTURE(line);
        CHECK_FALSE(egraph::parse_event(line).has_value());
    }
}

TEST_CASE("only a phase leaves its request going") {
    using Kind = egraph::WorkerEvent::Kind;
    CHECK_FALSE(egraph::final_event({.kind = Kind::phase, .text = "x", .status = 0, .log = ""}));
    for (const auto kind :
         {Kind::built, Kind::merged, Kind::uninstalled, Kind::failed, Kind::error}) {
        CHECK(egraph::final_event({.kind = kind, .text = "x", .status = 0, .log = ""}));
    }
    CHECK(egraph::describe_event(
              {.kind = Kind::failed, .text = "compile", .status = 1, .log = "/l"}) ==
          "compile failed with status 1 (log: /l)");
    CHECK(egraph::describe_event(
              {.kind = Kind::phase, .text = "compile", .status = 0, .log = ""}) == "compile");
    CHECK(egraph::describe_event({.kind = Kind::built, .text = "a/b-1", .status = 0, .log = ""}) ==
          "built");
    CHECK(egraph::describe_event(
              {.kind = Kind::error, .text = "no ebuild", .status = 0, .log = ""}) == "no ebuild");
}

TEST_CASE("a plan merging portage is told from one that does not") {
    const auto system = make_system({{.cpv = "sys-apps/portage-3.0"}, {.cpv = "app-misc/foo-1"}},
                                    {{.cpv = "sys-apps/portage-3.1"}, {.cpv = "app-misc/foo-1"}});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    CHECK(egraph::merges_cp(plan, system.evaluated, "sys-apps/portage"));
    CHECK_FALSE(egraph::merges_cp(plan, system.evaluated, "app-misc/foo"));
    CHECK_FALSE(egraph::merges_cp(egraph::Plan{}, system.evaluated, "sys-apps/portage"));
}
