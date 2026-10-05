#include "resume.hpp"

#include "plan.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
    CHECK(entry["myopts"] == Json::parse(R"({"--update": true, "--oneshot": true,
                                             "--regex-search-auto": "y"})"));
    CHECK(entry["favorites"] == Json::array());
    CHECK(entry["binpkgs"] == Json::array());
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
                                             "--reinstall": "changed-use", "--dynamic-deps": "n",
                                             "--regex-search-auto": "y"})"));
    CHECK(entry["favorites"] == Json::parse(R"(["app-misc/new", "@set"])"));
    const egraph::EmergeRequest newuse{.targets = {}, .rebuilds = egraph::UseRebuilds::all};
    CHECK(Json::parse(egraph::resume_entry(system.evaluated, plan, "/", newuse, false))["myopts"] ==
          Json::parse(R"({"--newuse": true, "--regex-search-auto": "y"})"));
}

namespace {

Json myopts(const std::vector<std::string>& options) {
    return Json::parse(egraph::emerge_myopts(options));
}

} // namespace

TEST_CASE("emerge keeps the roots given and drops --ask=n, with its search default") {
    CHECK(myopts({"--root=/", "--config-root=/c/", "--prefix=/p", "--ignore-default-opts",
                  "--ask=n"}) == Json::parse(R"({"--root": "/", "--config-root": "/c/",
                                                 "--prefix": "/p", "--ignore-default-opts": true,
                                                 "--regex-search-auto": "y"})"));
}

TEST_CASE("emerge keeps each execution option in its own type, as its parser leaves it") {
    CHECK(myopts({"--alert", "--buildpkg=n", "--fail-clean=y", "--keep-going=n", "--quiet=y",
                  "--quiet-build=True", "--quiet-fail=n", "--color=n", "--nospinner", "--jobs=2",
                  "--jobs-tmpdir-require-free-gb=0", "--load-average=4", "--buildpkg-exclude=a/b",
                  "--buildpkg-exclude=c/d e/f"}) ==
          Json::parse(R"({"--alert": true, "--buildpkg": "n", "--fail-clean": true,
                          "--quiet": true, "--quiet-build": "y", "--quiet-fail": "n",
                          "--color": "n", "--nospinner": true, "--jobs": 2,
                          "--jobs-tmpdir-require-free-gb": 0, "--load-average": 4.0,
                          "--buildpkg-exclude": ["a/b", "c/d e/f"],
                          "--regex-search-auto": "y"})"));
}

TEST_CASE("the last of an option decides, and what turns one off leaves it out") {
    const auto search = Json::parse(R"({"--regex-search-auto": "y"})");
    CHECK(myopts({"--jobs=3", "--jobs"}) ==
          Json::parse(R"({"--jobs": true, "--regex-search-auto": "y"})"));
    CHECK(myopts({"--jobs=3", "--jobs=n"}) == search);
    CHECK(myopts({"--keep-going", "--keep-going=n"}) == search);
    CHECK(myopts({"--alert=n", "--quiet=n", "--load-average=0"}) == search);
    CHECK(myopts({"--load-average"}) == search);
    CHECK(myopts({"--deep=2"}) == Json::parse(R"({"--deep": 2, "--regex-search-auto": "y"})"));
}

TEST_CASE("a request's options are kept as emerge's parser leaves them") {
    CHECK(myopts({"--update", "--deep", "--noreplace", "--newuse", "--changed-use",
                  "--dynamic-deps=n", "--oneshot"}) ==
          Json::parse(R"({"--update": true, "--deep": true, "--noreplace": true,
                          "--newuse": true, "--reinstall": "changed-use",
                          "--dynamic-deps": "n", "--oneshot": true,
                          "--regex-search-auto": "y"})"));
}

TEST_CASE("an entry lists the merges given, in their order, with the options and favorites") {
    const auto system = make_system(
        {}, {{.cpv = "app-misc/a-1"}, {.cpv = "app-misc/b-1"}, {.cpv = "app-misc/c-1"}});
    egraph::Plan plan;
    plan.merges.resize(3);
    for (std::uint32_t i = 0; i < 3; ++i) {
        plan.merges.at(i).candidate = i;
    }
    plan.order = {1, 2, 0};
    const std::array<std::size_t, 2> listed{plan.order.at(2), plan.order.at(0)};
    const std::vector<std::string> options{"--ignore-default-opts", "--keep-going"};
    const std::vector<std::string> favorites{"app-misc/a"};
    const auto entry = Json::parse(
        egraph::resume_entry(system.evaluated, plan, "/r/", listed, options, favorites));
    const auto cpv = [&](std::size_t index) {
        return system.evaluated.string(
            system.evaluated.candidates.at(plan.merges.at(plan.order.at(index)).candidate).cpv);
    };
    CHECK(entry["mergelist"] == Json::array({Json::array({"ebuild", "/r/", cpv(2), "merge"}),
                                             Json::array({"ebuild", "/r/", cpv(0), "merge"})}));
    CHECK(entry["myopts"] == Json::parse(R"({"--ignore-default-opts": true, "--keep-going": true,
                                             "--regex-search-auto": "y"})"));
    CHECK(entry["favorites"] == Json::parse(R"(["app-misc/a"])"));
    CHECK(entry["binpkgs"] == Json::array());
}

namespace {

// Merges m-0 to m-3, then an uninstall.
struct Run {
    egraph::Plan plan;
    std::vector<egraph::Step> steps;
};

Run run() {
    Run found;
    found.plan.merges.resize(4);
    found.plan.order = {0, 1, 2, 3};
    found.plan.uninstalls = {{.package = 0, .why = {}, .after = {}}};
    for (std::uint32_t i = 0; i < 4; ++i) {
        found.steps.emplace_back(egraph::MergeStep{.merge = i, .blockers = {}, .world = {}});
    }
    found.steps.emplace_back(egraph::UninstallStep{.uninstall = 0, .clean_world = false});
    return found;
}

void merge(egraph::Schedule& schedule, std::size_t step, bool succeeds = true) {
    schedule.build_started(step);
    schedule.build_finished(step, true);
    schedule.merge_started(step);
    schedule.merge_finished(step, succeeds);
}

void fail_build(egraph::Schedule& schedule, std::size_t step) {
    schedule.build_started(step);
    schedule.build_finished(step, false);
}

using Steps = std::vector<std::size_t>;

} // namespace

TEST_CASE("a run lists every merge, each dropped once merged, but no uninstall") {
    const auto [plan, steps] = run();
    egraph::Schedule schedule{plan, steps, 1};
    egraph::ResumeList list{schedule};
    CHECK(list.steps(schedule) == Steps{0, 1, 2, 3});
    merge(schedule, 0);
    CHECK(list.steps(schedule) == Steps{1, 2, 3});
}

TEST_CASE("without --keep-going, a failure leaves the list as it was, failed step and all") {
    const auto [plan, steps] = run();
    egraph::Schedule schedule{plan, steps, 1};
    egraph::ResumeList list{schedule};
    merge(schedule, 0);
    fail_build(schedule, 1);
    CHECK_FALSE(list.ended(schedule, false));
    CHECK(list.steps(schedule) == Steps{1, 2, 3});
}

TEST_CASE("--keep-going going on saves what is left, without the failed and skipped") {
    const auto [plan, steps] = run();
    egraph::Schedule schedule{plan, steps, 1};
    egraph::ResumeList list{schedule};
    fail_build(schedule, 0);
    const std::array<std::size_t, 1> skipped{2};
    CHECK(list.weighed(schedule, std::span<const std::size_t>{skipped}));
    CHECK(list.steps(schedule) == Steps{1, 3});
    schedule.resume(skipped);
    merge(schedule, 1);
    merge(schedule, 3);
    CHECK(list.steps(schedule).empty());
    CHECK_FALSE(list.ended(schedule, true));
}

TEST_CASE("what --keep-going drops without going on is saved at the end once a merge changed "
          "the system") {
    SECTION("a run that cannot go on, after a merge") {
        const auto [plan, steps] = run();
        egraph::Schedule schedule{plan, steps, 1};
        egraph::ResumeList list{schedule};
        merge(schedule, 0);
        fail_build(schedule, 1);
        CHECK_FALSE(list.weighed(schedule, std::nullopt));
        CHECK(list.steps(schedule) == Steps{2, 3});
        CHECK(list.ended(schedule, true));
    }
    SECTION("nothing installed changed") {
        const auto [plan, steps] = run();
        egraph::Schedule schedule{plan, steps, 1};
        egraph::ResumeList list{schedule};
        fail_build(schedule, 0);
        CHECK_FALSE(list.weighed(schedule, std::nullopt));
        CHECK_FALSE(list.ended(schedule, true));
    }
    SECTION("everything left skipped, which stays listed") {
        const auto [plan, steps] = run();
        egraph::Schedule schedule{plan, steps, 1};
        egraph::ResumeList list{schedule};
        merge(schedule, 0);
        fail_build(schedule, 1);
        const std::array<std::size_t, 2> skipped{2, 3};
        CHECK_FALSE(list.weighed(schedule, std::span<const std::size_t>{skipped}));
        CHECK(list.steps(schedule) == Steps{2, 3});
        schedule.resume(skipped);
        CHECK(list.ended(schedule, true));
    }
    SECTION("a failure in the last pass") {
        const auto [plan, steps] = run();
        egraph::Schedule schedule{plan, steps, 1};
        egraph::ResumeList list{schedule};
        merge(schedule, 0);
        merge(schedule, 1);
        merge(schedule, 2);
        fail_build(schedule, 3);
        CHECK(list.ended(schedule, true));
        CHECK(list.steps(schedule).empty());
    }
}
