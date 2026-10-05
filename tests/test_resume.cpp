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
        Json::parse(egraph::resume_entry(system.evaluated, plan, "/mnt/root/", request, true, {}));
    CHECK(entry["mergelist"] ==
          Json::parse(R"([["ebuild", "/mnt/root/", "dev-libs/fresh-1", "merge"],
                                                ["ebuild", "/mnt/root/", "app-misc/up-2", "merge"]])"));
    CHECK(entry["myopts"] == Json::parse(R"({"--update": true, "--oneshot": true,
                                             "--regex-search-auto": "y"})"));
    // emerge keeps them with --oneshot too.
    CHECK(entry["favorites"] == Json::parse(R"(["@installed"])"));
    CHECK(entry["binpkgs"] == Json::array());
}

TEST_CASE("the request's options are emerge's, and its targets favorites") {
    const auto system = make_system({}, {{.cpv = "app-misc/new-1"}});
    const egraph::Plan plan;
    // As parse_request reads the targets: the set's atoms named by it, an atom given its category.
    const std::vector<egraph::Argument> arguments{{.set = "", .atom = "app-misc/new"},
                                                  {.set = "set", .atom = "app-misc/member"}};
    const egraph::EmergeRequest request{.targets = {"new", "@set"},
                                        .update = true,
                                        .deep = true,
                                        .noreplace = true,
                                        .rebuilds = egraph::UseRebuilds::changed,
                                        .dynamic_deps = false};
    const auto entry =
        Json::parse(egraph::resume_entry(system.evaluated, plan, "/", request, false, arguments));
    CHECK(entry["mergelist"] == Json::array());
    // emerge keeps --changed-use as the --reinstall it stands for.
    CHECK(entry["myopts"] == Json::parse(R"({"--update": true, "--deep": true, "--noreplace": true,
                                             "--reinstall": "changed-use", "--dynamic-deps": "n",
                                             "--regex-search-auto": "y"})"));
    CHECK(entry["favorites"] == Json::parse(R"(["@set", "app-misc/new"])"));
    const egraph::EmergeRequest newuse{.targets = {}, .rebuilds = egraph::UseRebuilds::all};
    CHECK(Json::parse(
              egraph::resume_entry(system.evaluated, plan, "/", newuse, false, {}))["myopts"] ==
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
