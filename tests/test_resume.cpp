#include "resume.hpp"

#include "plan.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using egraph::test::make_system;
using Json = nlohmann::json;

TEST_CASE("a plan's merges are emerge's resume list, in the plan's order") {
    auto system = make_system({{.cpv = "app-misc/up-1"}},
                              {{.cpv = "app-misc/up-1"},
                               {.cpv = "app-misc/up-2", .deps = {{"RDEPEND", "dev-libs/fresh"}}},
                               {.cpv = "dev-libs/fresh-1"}});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    const egraph::EmergeRequest request{.targets = {"@installed"}, .update = true};
    const auto entry =
        Json::parse(egraph::resume_entry(system.evaluated, plan, "/mnt/root/", request, true));
    CHECK(entry["mergelist"] ==
          Json::parse(R"([["ebuild", "/mnt/root/", "dev-libs/fresh-1", "merge"],
                                                ["ebuild", "/mnt/root/", "app-misc/up-2", "merge"]])"));
    CHECK(entry["myopts"] == Json::parse(R"({"--update": true, "--oneshot": true})"));
    CHECK(entry["favorites"] == Json::array());
}

TEST_CASE("the request's options are emerge's, and its targets favorites unless oneshot") {
    const auto system = make_system({}, {{.cpv = "app-misc/new-1"}});
    const egraph::Plan plan;
    const egraph::EmergeRequest request{.targets = {"app-misc/new", "@set"},
                                        .update = true,
                                        .deep = true,
                                        .noreplace = true,
                                        .rebuilds = egraph::UseRebuilds::changed,
                                        .dynamic_deps = false};
    const auto entry =
        Json::parse(egraph::resume_entry(system.evaluated, plan, "/", request, false));
    CHECK(entry["mergelist"] == Json::array());
    // emerge keeps --changed-use as the --reinstall it stands for.
    CHECK(entry["myopts"] == Json::parse(R"({"--update": true, "--deep": true, "--noreplace": true,
                                             "--reinstall": "changed-use", "--dynamic-deps": "n"})"));
    CHECK(entry["favorites"] == Json::parse(R"(["app-misc/new", "@set"])"));
    const egraph::EmergeRequest newuse{.targets = {}, .rebuilds = egraph::UseRebuilds::all};
    CHECK(Json::parse(egraph::resume_entry(system.evaluated, plan, "/", newuse, false))["myopts"] ==
          Json::parse(R"({"--newuse": true})"));
}
