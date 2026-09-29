#include "plan.hpp"
#include "query.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

using egraph::test::Available;
using egraph::test::Installed;
using egraph::test::make_system;

namespace {

std::string member(const egraph::test::System& system, const egraph::Member& member) {
    return std::string(
        member.candidate ? system.evaluated.string(system.evaluated.candidates.at(member.index).cpv)
                         : system.store.string(system.store.packages.at(member.index).cpv));
}

std::string reason(const egraph::test::System& system, const egraph::Reason& reason) {
    return std::format("{} {}",
                       member(system, egraph::Member{.candidate = reason.member.candidate,
                                                     .index = reason.member.index}),
                       reason.atom);
}

// "installed -> target", "installed -> installed for merge atom", "new cpv <- member atom",
// "installed held <- member atom; ...".
std::vector<std::string> plan(const egraph::test::System& system,
                              egraph::UseRebuilds rebuilds = egraph::UseRebuilds::none,
                              const std::vector<bool>& scope = {}) {
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, rebuilds, scope);
    std::vector<std::string> lines;
    for (const auto& merge : found.merges) {
        const auto target = evaluated.string(evaluated.candidates.at(merge.candidate).cpv);
        if (merge.replaces) {
            auto line = std::format("{} -> {}",
                                    store.string(store.packages.at(*merge.replaces).cpv), target);
            if (merge.rebuilt_for) {
                line += std::format(" for {}", reason(system, *merge.rebuilt_for));
            }
            lines.push_back(std::move(line));
        } else {
            lines.push_back(std::format("new {} <- {}", target,
                                        merge.pulled_by ? reason(system, *merge.pulled_by) : ""));
        }
    }
    for (const auto& held : found.held) {
        auto line = std::format("{} held <-", store.string(store.packages.at(held.package).cpv));
        for (std::size_t i = 0; i < held.reasons.size(); ++i) {
            line += std::format("{} {}", i == 0 ? "" : ";", reason(system, held.reasons.at(i)));
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

TEST_CASE("an update with nothing in its way is merged") {
    const auto system = make_system({{.cpv = "app-misc/up-1"}},
                                    {{.cpv = "app-misc/up-1"}, {.cpv = "app-misc/up-2"}});
    CHECK(plan(system) == std::vector<std::string>{"app-misc/up-1 -> app-misc/up-2"});
}

TEST_CASE("a target's own dependencies pull in what nothing installed provides") {
    const auto system =
        make_system({{.cpv = "app-misc/glibmm-1"}},
                    {{.cpv = "app-misc/glibmm-1"},
                     {.cpv = "app-misc/glibmm-2", .deps = {{"BDEPEND", "dev-cpp/mm-common"}}},
                     {.cpv = "dev-cpp/mm-common-1", .deps = {{"RDEPEND", "dev-libs/chain"}}},
                     {.cpv = "dev-cpp/mm-common-1.1", .deps = {{"RDEPEND", "dev-libs/chain"}}},
                     {.cpv = "dev-libs/chain-1"},
                     {.cpv = "dev-libs/chain-2", .visible = false}});
    CHECK(plan(system) == std::vector<std::string>{
                              "app-misc/glibmm-1 -> app-misc/glibmm-2",
                              "new dev-cpp/mm-common-1.1 <- app-misc/glibmm-2 dev-cpp/mm-common",
                              "new dev-libs/chain-1 <- dev-cpp/mm-common-1.1 dev-libs/chain"});
}

TEST_CASE("a || group takes its first alternative, unless something installed satisfies it") {
    const auto system = make_system(
        {{.cpv = "app-misc/choose-1"}, {.cpv = "app-misc/either-1"}, {.cpv = "dev-libs/present-1"}},
        {{.cpv = "app-misc/choose-1"},
         {.cpv = "app-misc/choose-2",
          .deps = {{"RDEPEND", "|| ( dev-libs/first dev-libs/second )"}}},
         {.cpv = "app-misc/either-1"},
         {.cpv = "app-misc/either-2",
          .deps = {{"RDEPEND", "|| ( dev-libs/absent dev-libs/present )"}}},
         {.cpv = "dev-libs/first-1"},
         {.cpv = "dev-libs/second-1"},
         {.cpv = "dev-libs/absent-1"},
         {.cpv = "dev-libs/present-1"}});
    CHECK(plan(system) ==
          std::vector<std::string>{"app-misc/choose-1 -> app-misc/choose-2",
                                   "app-misc/either-1 -> app-misc/either-2",
                                   "new dev-libs/first-1 <- app-misc/choose-2 dev-libs/first"});
}

TEST_CASE("a slot nothing occupies is pulled in beside the installed one") {
    const auto system =
        make_system({{.cpv = "app-misc/slotty-1", .deps = {{"RDEPEND", "dev-lang/py:3.13"}}},
                     {.cpv = "dev-lang/py-3.13.1", .slot = "3.13"}},
                    {{.cpv = "app-misc/slotty-1", .deps = {{"RDEPEND", "dev-lang/py:3.13"}}},
                     {.cpv = "app-misc/slotty-2", .deps = {{"RDEPEND", "dev-lang/py:3.14"}}},
                     {.cpv = "dev-lang/py-3.13.1", .slot = "3.13"},
                     {.cpv = "dev-lang/py-3.14.1", .slot = "3.14"}});
    CHECK(plan(system) ==
          std::vector<std::string>{"app-misc/slotty-1 -> app-misc/slotty-2",
                                   "new dev-lang/py-3.14.1 <- app-misc/slotty-2 dev-lang/py:3.14"});
}

TEST_CASE("a kept package's dependencies pull in what they lack, build-time ones too") {
    const auto system = make_system(
        {{.cpv = "app-misc/grown-1",
          .deps = {{"RDEPEND", "dev-libs/fresh"}, {"BDEPEND", "dev-libs/tool"}}}},
        {{.cpv = "app-misc/grown-1"}, {.cpv = "dev-libs/fresh-1"}, {.cpv = "dev-libs/tool-1"}});
    CHECK(plan(system) ==
          std::vector<std::string>{"new dev-libs/fresh-1 <- app-misc/grown-1 dev-libs/fresh",
                                   "new dev-libs/tool-1 <- app-misc/grown-1 dev-libs/tool"});
    // Out of scope, they pull nothing in.
    CHECK(plan(system, egraph::UseRebuilds::none, {false}).empty());
}

TEST_CASE("an update that needs a held one is held by its own dependency") {
    const auto system =
        make_system({{.cpv = "app-misc/host-1"},
                     {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<app-misc/host-2"}}},
                     {.cpv = "app-misc/plugin-1", .deps = {{"RDEPEND", "app-misc/host"}}}},
                    {{.cpv = "app-misc/host-1"},
                     {.cpv = "app-misc/host-2"},
                     {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<app-misc/host-2"}}},
                     {.cpv = "app-misc/plugin-1", .deps = {{"RDEPEND", "app-misc/host"}}},
                     {.cpv = "app-misc/plugin-2", .deps = {{"RDEPEND", ">=app-misc/host-2"}}}});
    CHECK(plan(system) == std::vector<std::string>{
                              "app-misc/host-1 held <- app-misc/holder-1 <app-misc/host-2",
                              "app-misc/plugin-1 held <- app-misc/plugin-2 >=app-misc/host-2"});
}

TEST_CASE("a target's own dependency holds another update back to a version it accepts") {
    const auto system =
        make_system({{.cpv = "app-misc/strict-1"}, {.cpv = "dev-libs/moving-1"}},
                    {{.cpv = "app-misc/strict-1"},
                     {.cpv = "app-misc/strict-2", .deps = {{"RDEPEND", "<dev-libs/moving-2"}}},
                     {.cpv = "dev-libs/moving-1"},
                     {.cpv = "dev-libs/moving-1.5"},
                     {.cpv = "dev-libs/moving-2"}});
    CHECK(plan(system) == std::vector<std::string>{
                              "app-misc/strict-1 -> app-misc/strict-2",
                              "dev-libs/moving-1 -> dev-libs/moving-1.5",
                              "dev-libs/moving-1 held <- app-misc/strict-2 <dev-libs/moving-2"});
}

TEST_CASE("a dependent's || takes another alternative rather than hold an update") {
    const auto system =
        make_system({{.cpv = "app-misc/either-1",
                      .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
                     {.cpv = "dev-libs/alt-1"}},
                    {{.cpv = "app-misc/either-1",
                      .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
                     {.cpv = "dev-libs/alt-1"},
                     {.cpv = "dev-libs/alt-2"},
                     {.cpv = "dev-libs/other-1"}});
    CHECK(plan(system) ==
          std::vector<std::string>{"dev-libs/alt-1 -> dev-libs/alt-2",
                                   "new dev-libs/other-1 <- app-misc/either-1 dev-libs/other"});
}

TEST_CASE("dependents hold an update back to the best version they all accept") {
    const auto system =
        make_system({{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
                     {.cpv = "dev-libs/astroid-4.0.4"}},
                    {{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
                     {.cpv = "dev-libs/astroid-4.0.4"},
                     {.cpv = "dev-libs/astroid-4.0.5"},
                     {.cpv = "dev-libs/astroid-4.3.2"}});
    CHECK(plan(system) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0.4 -> dev-libs/astroid-4.0.5",
              "dev-libs/astroid-4.0.4 held <- app-misc/pylint-1 <dev-libs/astroid-4.1"});
    // Out of scope, a dependent holds nothing back.
    CHECK(plan(system, egraph::UseRebuilds::none, {false, true}) ==
          std::vector<std::string>{"dev-libs/astroid-4.0.4 -> dev-libs/astroid-4.3.2"});
}

TEST_CASE("a new sub-slot rebuilds the dependents bound to the old one") {
    const auto system =
        make_system({{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "app-misc/ddep-1", .deps = {{"DEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "app-misc/pdep-1", .deps = {{"PDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "app-misc/star-1", .deps = {{"RDEPEND", "dev-libs/lib:*"}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/ddep-1", .deps = {{"DEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/pdep-1", .deps = {{"PDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/star-1", .deps = {{"RDEPEND", "dev-libs/lib:*"}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .sub_slot = "2"}});
    CHECK(plan(system) ==
          std::vector<std::string>{
              "app-misc/rdep-1 -> app-misc/rdep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "app-misc/ddep-1 -> app-misc/ddep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "app-misc/pdep-1 -> app-misc/pdep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "dev-libs/lib-1 -> dev-libs/lib-2"});
    // Out of scope, a dependent is not rebuilt.
    CHECK(plan(system, egraph::UseRebuilds::none, {false, true, true, true, true}) ==
          std::vector<std::string>{
              "app-misc/ddep-1 -> app-misc/ddep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "app-misc/pdep-1 -> app-misc/pdep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "dev-libs/lib-1 -> dev-libs/lib-2"});
}

TEST_CASE("a dependent that is updated is not rebuilt as well") {
    const auto system =
        make_system({{.cpv = "app-misc/moving-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/moving-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/moving-2", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .sub_slot = "2"}});
    CHECK(plan(system) == std::vector<std::string>{"app-misc/moving-1 -> app-misc/moving-2",
                                                   "dev-libs/lib-1 -> dev-libs/lib-2"});
}

TEST_CASE("a merge in the bound sub-slot needs no rebuild") {
    const auto system =
        make_system({{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-1.1", .sub_slot = "1"}});
    CHECK(plan(system) == std::vector<std::string>{"dev-libs/lib-1 -> dev-libs/lib-1.1"});
}

TEST_CASE("a dependent with no ebuild to rebuild from holds the update to its sub-slot") {
    const auto system = make_system(
        {{.cpv = "app-misc/gone-1", .deps = {{"RDEPEND", "dev-libs/lone:0/1="}}},
         {.cpv = "dev-libs/lone-1", .sub_slot = "1"}},
        {{.cpv = "app-misc/gone-1", .deps = {{"RDEPEND", "dev-libs/lone:="}}, .visible = false},
         {.cpv = "dev-libs/lone-1", .sub_slot = "1"},
         {.cpv = "dev-libs/lone-1.5", .sub_slot = "1"},
         {.cpv = "dev-libs/lone-2", .sub_slot = "2"}});
    CHECK(plan(system) ==
          std::vector<std::string>{"dev-libs/lone-1 -> dev-libs/lone-1.5",
                                   "dev-libs/lone-1 held <- app-misc/gone-1 dev-libs/lone:0/1="});
}

TEST_CASE("a rebuild pulls in what its ebuild needs, or holds the update when nothing can") {
    const auto system = make_system(
        {{.cpv = "app-misc/grown-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
         {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
         {.cpv = "app-misc/stuck-1", .deps = {{"RDEPEND", "dev-libs/solo:0/1="}}},
         {.cpv = "dev-libs/solo-1", .sub_slot = "1"}},
        {{.cpv = "app-misc/grown-1", .deps = {{"RDEPEND", "dev-libs/lib:= dev-libs/fresh"}}},
         {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
         {.cpv = "dev-libs/lib-2", .sub_slot = "2"},
         {.cpv = "dev-libs/fresh-1"},
         {.cpv = "app-misc/stuck-1", .deps = {{"RDEPEND", "dev-libs/solo:= dev-libs/nowhere"}}},
         {.cpv = "dev-libs/solo-1", .sub_slot = "1"},
         {.cpv = "dev-libs/solo-2", .sub_slot = "2"}});
    CHECK(plan(system) ==
          std::vector<std::string>{
              "app-misc/grown-1 -> app-misc/grown-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "dev-libs/lib-1 -> dev-libs/lib-2",
              "new dev-libs/fresh-1 <- app-misc/grown-1 dev-libs/fresh",
              "dev-libs/solo-1 held <- app-misc/stuck-1 dev-libs/nowhere"});
}

TEST_CASE("a target whose dependency nothing satisfies is held by it") {
    const auto system =
        make_system({{.cpv = "app-misc/lost-1"}},
                    {{.cpv = "app-misc/lost-1"},
                     {.cpv = "app-misc/lost-2", .deps = {{"RDEPEND", "dev-libs/nowhere"}}}});
    CHECK(plan(system) ==
          std::vector<std::string>{"app-misc/lost-1 held <- app-misc/lost-2 dev-libs/nowhere"});
}

TEST_CASE("an update that loses its dependency on a pulled package no longer pulls it") {
    // lost-2 cannot be merged, so the package only it pulled in is not merged either.
    const auto system = make_system(
        {{.cpv = "app-misc/lost-1"}},
        {{.cpv = "app-misc/lost-1"},
         {.cpv = "app-misc/lost-2", .deps = {{"RDEPEND", "dev-libs/extra dev-libs/nowhere"}}},
         {.cpv = "dev-libs/extra-1"}});
    CHECK(plan(system) ==
          std::vector<std::string>{"app-misc/lost-1 held <- app-misc/lost-2 dev-libs/nowhere"});
}

TEST_CASE("update lines name the merge a slot-operator rebuild is for") {
    const auto system =
        make_system({{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .sub_slot = "2"}});
    CHECK(egraph::update_lines(system.store, system.evaluated, egraph::UseRebuilds::none) ==
          std::vector<std::string>{
              "app-misc/rdep-1\trebuild\tapp-misc/rdep-1\ttest_repo\t\tdev-libs/lib-2 "
              "dev-libs/lib:0/1=",
              "dev-libs/lib-1\tupgrade\tdev-libs/lib-2\ttest_repo"});
}
