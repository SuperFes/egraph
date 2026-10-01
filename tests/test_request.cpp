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
