#include "request.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::test::add_repository_cps;
using egraph::test::make_system;

namespace {

egraph::test::System sample() {
    return make_system({{.cpv = "app-misc/tool-1"},
                        {.cpv = "dev-libs/lib-1", .deps = {{"RDEPEND", "dev-libs/deep"}}},
                        {.cpv = "dev-libs/deep-1"},
                        {.cpv = "dev-libs/apart-1"}},
                       {{.cpv = "app-misc/tool-1"},
                        {.cpv = "app-misc/tool-2"},
                        {.cpv = "app-misc/fresh-1", .iuse = "gtk"},
                        {.cpv = "dev-libs/lib-1"},
                        {.cpv = "dev-python/lib-1"},
                        {.cpv = "app-misc/hidden-1", .visible = false}},
                       {"app-misc/tool"}, {"dev-libs/lib"});
}

std::expected<egraph::Request, std::string> parse(const egraph::test::System& system,
                                                  std::vector<std::string> words) {
    return egraph::parse_request(system.store, system.evaluated, words);
}

} // namespace

TEST_CASE("a request's atoms are its arguments, alone") {
    const auto system = sample();
    const auto found = parse(system, {"app-misc/tool", ">=app-misc/fresh-1", "=dev-libs/lib-1"});
    REQUIRE(found);
    CHECK_FALSE(found->installed);
    CHECK(found->arguments ==
          std::vector<egraph::Argument>{{.set = "", .atom = "app-misc/tool"},
                                        {.set = "", .atom = ">=app-misc/fresh-1"},
                                        {.set = "", .atom = "=dev-libs/lib-1"}});
}

TEST_CASE("a set expands to the root atoms the store holds for it") {
    const auto system = sample();
    CHECK(parse(system, {"@selected"})->arguments ==
          std::vector<egraph::Argument>{{.set = "selected", .atom = "app-misc/tool"}});
    CHECK(parse(system, {"@world"})->arguments ==
          std::vector<egraph::Argument>{{.set = "selected", .atom = "app-misc/tool"},
                                        {.set = "system", .atom = "dev-libs/lib"}});
    const auto installed = parse(system, {"@installed"});
    REQUIRE(installed);
    CHECK(installed->installed);
    CHECK(installed->arguments.empty());
    CHECK(parse(system, {"@preserved-rebuild"}).error() ==
          "@preserved-rebuild: no such set in the store");
}

TEST_CASE("a set the caller gives expands to its atoms, named by it") {
    const auto system = sample();
    const egraph::Sets given{{"preserved-rebuild", {"dev-libs/lib:0", "app-misc/tool:0"}}};
    const std::vector<std::string> words{"@preserved-rebuild", "app-misc/fresh"};
    CHECK(egraph::parse_request(system.store, system.evaluated, words, given)->arguments ==
          std::vector<egraph::Argument>{{.set = "preserved-rebuild", .atom = "dev-libs/lib:0"},
                                        {.set = "preserved-rebuild", .atom = "app-misc/tool:0"},
                                        {.set = "", .atom = "app-misc/fresh"}});
}

TEST_CASE("a name without a category takes the one the stores know it in") {
    const auto system = sample();
    CHECK(parse(system, {"fresh"})->arguments ==
          std::vector<egraph::Argument>{{.set = "", .atom = "app-misc/fresh"}});
    CHECK(parse(system, {">=tool-2:0"})->arguments ==
          std::vector<egraph::Argument>{{.set = "", .atom = ">=app-misc/tool-2:0"}});
    CHECK(parse(system, {"deep"})->arguments ==
          std::vector<egraph::Argument>{{.set = "", .atom = "dev-libs/deep"}});
    CHECK(parse(system, {"lib"}).error() == "lib: ambiguous, in dev-libs/lib and dev-python/lib");
    CHECK(parse(system, {"nowhere"}).error() ==
          "nowhere: no package by that name in the repositories");
}

TEST_CASE("a name without a category takes the one the repositories know it in") {
    auto system = sample();
    add_repository_cps(system, {"net-misc/elsewhere", "www-apps/lib"});
    const auto elsewhere = parse(system, {"elsewhere"});
    REQUIRE(elsewhere);
    CHECK(elsewhere->arguments ==
          std::vector<egraph::Argument>{{.set = "", .atom = "net-misc/elsewhere"}});
    CHECK(parse(system, {"lib"}).error() ==
          "lib: ambiguous, in dev-libs/lib, dev-python/lib and www-apps/lib");
}

TEST_CASE("a name takes its one category beside virtuals and accounts, as emerge does") {
    auto system = sample();
    add_repository_cps(system,
                       {"virtual/tool", "acct-user/tool", "virtual/only", "acct-group/only"});
    CHECK(parse(system, {"tool"})->arguments ==
          std::vector<egraph::Argument>{{.set = "", .atom = "app-misc/tool"}});
    CHECK(parse(system, {"only"}).error() ==
          "only: ambiguous, in acct-group/only and virtual/only");
}

TEST_CASE("a cp only the repositories know is left for the builder to evaluate") {
    auto system = sample();
    add_repository_cps(system, {"net-misc/elsewhere"});
    const auto request =
        parse(system, {"net-misc/elsewhere", ">=net-misc/elsewhere-2", "app-misc/tool"});
    REQUIRE(request);
    CHECK(request->arguments.size() == 3);
    CHECK(request->unevaluated == std::vector<std::string>{"net-misc/elsewhere"});
    CHECK(parse(system, {"app-misc/tool"})->unevaluated.empty());
    CHECK(parse(system, {"net-misc/nowhere"}).error() == "net-misc/nowhere: nothing matches");
}

TEST_CASE("a cp evaluated on request with no ebuild visible is not evaluated again") {
    auto system = sample();
    add_repository_cps(system, {"net-misc/allmasked"});
    egraph::test::set_requested(system, {"net-misc/allmasked"});
    CHECK(parse(system, {"net-misc/allmasked"}).error() ==
          "net-misc/allmasked: nothing visible matches");
}

TEST_CASE("an atom nothing installed or visible matches is refused") {
    const auto system = sample();
    CHECK(parse(system, {"app-misc/hidden"}).error() ==
          "app-misc/hidden: every ebuild that matches is masked");
    CHECK(parse(system, {"app-misc/tool-2"}).error() ==
          "app-misc/tool-2: nothing matches; =app-misc/tool-2 names that version");
    CHECK(parse(system, {">=app-misc/tool-3"}).error() == ">=app-misc/tool-3: nothing matches");
    CHECK(parse(system, {"!app-misc/tool"}).error().starts_with("!app-misc/tool: "));
    // Installed without an ebuild is enough.
    CHECK(parse(system, {"dev-libs/apart"}));
    // So is what a USE change could meet; a flag outside IUSE no change meets later.
    CHECK(parse(system, {"app-misc/fresh[gtk]"}));
    CHECK(parse(system, {"app-misc/fresh[nope]"}));
    CHECK(parse(system, {">=app-misc/fresh-2[gtk]"}).error() ==
          ">=app-misc/fresh-2[gtk]: nothing matches");
}

TEST_CASE("a request reaches its installed packages and what they depend on") {
    const auto system = sample();
    const auto request = parse(system, {"dev-libs/lib"});
    REQUIRE(request);
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{false, true, true, false});
}

TEST_CASE("a request reaches what its atoms' ebuilds depend on") {
    const auto system =
        make_system({{.cpv = "dev-libs/used-1"}, {.cpv = "dev-libs/other-1"}},
                    {{.cpv = "app-misc/fresh-1", .deps = {{"RDEPEND", "dev-libs/used"}}},
                     {.cpv = "dev-libs/used-1"},
                     {.cpv = "dev-libs/other-1"}});
    const auto request = parse(system, {"app-misc/fresh"});
    REQUIRE(request);
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{true, false});
}

TEST_CASE("a request reaches no further than the versions its atoms accept") {
    const auto system =
        make_system({{.cpv = "app-misc/plain-1"}, {.cpv = "dev-libs/idle-1"}},
                    {{.cpv = "app-misc/plain-1"},
                     {.cpv = "app-misc/plain-2", .deps = {{"RDEPEND", "dev-libs/idle"}}},
                     {.cpv = "dev-libs/idle-1"}});
    const auto pinned = parse(system, {"=app-misc/plain-1"});
    REQUIRE(pinned);
    CHECK(egraph::request_reach(system.store, system.evaluated, *pinned) ==
          std::vector<bool>{true, false});
    const auto any = parse(system, {"app-misc/plain"});
    REQUIRE(any);
    CHECK(egraph::request_reach(system.store, system.evaluated, *any) ==
          std::vector<bool>{true, true});
}

TEST_CASE("a request reaches what a USE change could have its atoms merge") {
    const auto system = make_system(
        {{.cpv = "dev-libs/lib-1", .iuse = "gtk"}, {.cpv = "dev-libs/idle-1"}},
        {{.cpv = "dev-libs/lib-1", .iuse = "gtk"},
         {.cpv = "dev-libs/lib-2", .deps = {{"RDEPEND", "dev-libs/idle"}}, .iuse = "gtk"}});
    const auto request = parse(system, {"dev-libs/lib[gtk]"});
    REQUIRE(request);
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{true, true});
}

TEST_CASE("a request reaches what an update depends on, not what the version it replaces did") {
    const auto system =
        make_system({{.cpv = "app-misc/gains-1"},
                     {.cpv = "app-misc/drops-1", .deps = {{"RDEPEND", "dev-libs/old"}}},
                     {.cpv = "dev-libs/orphan-1"},
                     {.cpv = "dev-libs/old-1"}},
                    {{.cpv = "app-misc/gains-1"},
                     {.cpv = "app-misc/gains-2", .deps = {{"RDEPEND", "dev-libs/orphan"}}},
                     {.cpv = "app-misc/drops-1", .deps = {{"RDEPEND", "dev-libs/old"}}},
                     {.cpv = "app-misc/drops-2"},
                     {.cpv = "dev-libs/orphan-1"},
                     {.cpv = "dev-libs/old-1"}});
    const auto request = parse(system, {"app-misc/gains", "app-misc/drops"});
    REQUIRE(request);
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{true, true, true, false});
    // An argument its atom keeps at the installed version keeps that version's dependencies.
    const auto pinned = parse(system, {"=app-misc/drops-1"});
    REQUIRE(pinned);
    CHECK(egraph::request_reach(system.store, system.evaluated, *pinned) ==
          std::vector<bool>{false, true, false, true});
}

TEST_CASE("a request reaches what a new package it pulls in depends on") {
    const auto system =
        make_system({{.cpv = "app-misc/top-1"}, {.cpv = "dev-libs/deep-1"}},
                    {{.cpv = "app-misc/top-1"},
                     {.cpv = "app-misc/top-2", .deps = {{"RDEPEND", "dev-libs/fresh"}}},
                     {.cpv = "dev-libs/fresh-1", .deps = {{"RDEPEND", "dev-libs/deep"}}},
                     {.cpv = "dev-libs/deep-1"}});
    const auto request = parse(system, {"app-misc/top"});
    REQUIRE(request);
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{true, true});
}

TEST_CASE("a request reaches one alternative of a ||, one already reached before the first") {
    const auto system = make_system(
        {{.cpv = "app-misc/either-1", .deps = {{"RDEPEND", "|| ( dev-libs/x dev-libs/y )"}}},
         {.cpv = "app-misc/other-1", .deps = {{"RDEPEND", "dev-libs/z"}}},
         {.cpv = "dev-libs/x-1"},
         {.cpv = "dev-libs/y-1"},
         {.cpv = "dev-libs/z-1", .deps = {{"RDEPEND", "dev-libs/y"}}}},
        {{.cpv = "app-misc/either-1", .deps = {{"RDEPEND", "|| ( dev-libs/x dev-libs/y )"}}},
         {.cpv = "app-misc/other-1", .deps = {{"RDEPEND", "dev-libs/z"}}},
         {.cpv = "dev-libs/x-1"},
         {.cpv = "dev-libs/y-1"},
         {.cpv = "dev-libs/z-1", .deps = {{"RDEPEND", "dev-libs/y"}}}});
    const auto alone = parse(system, {"app-misc/either"});
    REQUIRE(alone);
    CHECK(egraph::request_reach(system.store, system.evaluated, *alone) ==
          std::vector<bool>{true, false, true, false, false});
    // The || waits until the plain dependencies are in, as emerge's do.
    const auto both = parse(system, {"app-misc/either", "app-misc/other"});
    REQUIRE(both);
    CHECK(egraph::request_reach(system.store, system.evaluated, *both) ==
          std::vector<bool>{true, true, false, true, true});
}

TEST_CASE("a request reaches the first alternative of a || that is installed, else visible") {
    const auto system = make_system(
        {{.cpv = "app-misc/either-1",
          .deps = {{"RDEPEND", "|| ( dev-libs/gone dev-libs/fresh dev-libs/y )"}}},
         {.cpv = "app-misc/new-1", .deps = {{"RDEPEND", "|| ( dev-libs/gone dev-libs/fresh )"}}},
         {.cpv = "dev-libs/y-1"},
         {.cpv = "dev-libs/deep-1"}},
        {{.cpv = "app-misc/either-1",
          .deps = {{"RDEPEND", "|| ( dev-libs/gone dev-libs/fresh dev-libs/y )"}}},
         {.cpv = "app-misc/new-1", .deps = {{"RDEPEND", "|| ( dev-libs/gone dev-libs/fresh )"}}},
         {.cpv = "dev-libs/fresh-1", .deps = {{"RDEPEND", "dev-libs/deep"}}},
         {.cpv = "dev-libs/y-1"},
         {.cpv = "dev-libs/deep-1"}});
    const auto installed = parse(system, {"app-misc/either"});
    REQUIRE(installed);
    CHECK(egraph::request_reach(system.store, system.evaluated, *installed) ==
          std::vector<bool>{true, false, true, false});
    const auto visible = parse(system, {"app-misc/new"});
    REQUIRE(visible);
    CHECK(egraph::request_reach(system.store, system.evaluated, *visible) ==
          std::vector<bool>{false, true, false, true});
}

TEST_CASE("a request leaves the update out of an installed version only a USE change lets match") {
    const auto system = make_system(
        {{.cpv = "dev-libs/lib-1", .iuse = "gtk"}, {.cpv = "dev-libs/deep-1"}},
        {{.cpv = "app-misc/wantold-1", .deps = {{"RDEPEND", "<dev-libs/lib-2[gtk]"}}},
         {.cpv = "dev-libs/lib-1", .deps = {{"RDEPEND", "dev-libs/deep"}}, .iuse = "gtk"},
         {.cpv = "dev-libs/lib-2", .iuse = "gtk"},
         {.cpv = "dev-libs/deep-1"}});
    const auto request = parse(system, {"app-misc/wantold"});
    REQUIRE(request);
    // The rebuild's own dependencies are followed.
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{false, true});
}

TEST_CASE("a request passes over an alternative only a USE change would satisfy") {
    const auto system =
        make_system({{.cpv = "dev-libs/lib-1", .iuse = "gtk"}, {.cpv = "dev-libs/deep-1"}},
                    {{.cpv = "app-misc/anyof-1",
                      .deps = {{"RDEPEND", "|| ( dev-libs/lib[gtk] dev-libs/other )"}}},
                     {.cpv = "dev-libs/lib-1", .iuse = "gtk"},
                     {.cpv = "dev-libs/lib-2", .iuse = "gtk"},
                     {.cpv = "dev-libs/other-1", .deps = {{"RDEPEND", "dev-libs/deep"}}},
                     {.cpv = "dev-libs/deep-1"}});
    const auto request = parse(system, {"app-misc/anyof"});
    REQUIRE(request);
    CHECK(egraph::request_reach(system.store, system.evaluated, *request) ==
          std::vector<bool>{false, true});
}
