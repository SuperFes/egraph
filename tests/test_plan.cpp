#include "plan.hpp"
#include "query.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
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
                              const egraph::Targets& targets = {}) {
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, rebuilds, targets);
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
    for (const auto& each : found.unsatisfied) {
        lines.push_back(std::format("unsatisfied {} {}",
                                    each.member ? member(system, *each.member) : "argument",
                                    each.atom));
    }
    for (const auto each : found.unmet) {
        lines.push_back(
            std::format("unmet {}", evaluated.string(evaluated.candidates.at(each).cpv)));
    }
    for (const auto& each : found.use_changes) {
        auto line = std::format(
            "use {}", evaluated.string(evaluated.candidates.at(each.change.candidate).cpv));
        for (const auto& [flag, on] : each.change.flags) {
            line += std::format(" {}{}", on ? "" : "-", flag);
        }
        line += std::format(" <- {}",
                            each.pulled_by ? reason(system, *each.pulled_by) : each.named_by->atom);
        lines.push_back(std::move(line));
    }
    return lines;
}

// Plain emerge with atoms as its arguments.
egraph::Targets reinstall(const std::vector<std::string>& atoms, bool deep = false) {
    egraph::Targets targets{.scope = {}, .roots = true, .deep = deep};
    for (const auto& atom : atoms) {
        targets.request.push_back({.set = "", .atom = atom});
    }
    targets.selection = egraph::Selection::reinstall;
    return targets;
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

TEST_CASE("-uD moves a dependency naming no slot to a newer slot beside the installed one") {
    // As virtual/wine's || ( ... app-emulation/wine-vanilla[wow64(-)] ... ): a satisfied atom
    // goes to its best visible version, whatever slot it is in.
    const std::vector<std::pair<std::string, std::string>> dependents{
        {"app-misc/plain-1", "app-misc/w"},
        {"app-misc/either-1", "|| ( app-misc/z[abi] app-misc/z )"},
        {"app-misc/stuck-1", "app-misc/v"},
        {"app-misc/older-1", "app-misc/y"},
        {"app-misc/ranged-1", "<app-misc/r-2"},
        {"app-misc/pinned-1", "app-misc/p:1"}};
    std::vector<Installed> installed{
        {.cpv = "app-misc/w-1", .slot = "1"}, {.cpv = "app-misc/z-1", .slot = "1", .iuse = "abi"},
        {.cpv = "app-misc/v-1", .slot = "1"}, {.cpv = "app-misc/y-3", .slot = "1"},
        {.cpv = "app-misc/r-1", .slot = "1"}, {.cpv = "app-misc/p-1", .slot = "1"}};
    std::vector<Available> available{
        {.cpv = "app-misc/w-1", .slot = "1"},
        {.cpv = "app-misc/w-2", .slot = "2"},
        {.cpv = "app-misc/z-1", .slot = "1", .iuse = "abi"},
        {.cpv = "app-misc/z-2", .slot = "2", .iuse = "abi"},
        {.cpv = "app-misc/v-1", .slot = "1"},
        // Nothing satisfies its dependency: emerge masks it and keeps v-1.
        {.cpv = "app-misc/v-2", .deps = {{"RDEPEND", "app-misc/missing"}}, .slot = "2"},
        // y-3 has no ebuild left, which emerge passes over for y-2 once that matched, unless
        // y-3 is in its graph already as an argument.
        {.cpv = "app-misc/y-2", .slot = "2"},
        {.cpv = "app-misc/r-1", .slot = "1"},
        {.cpv = "app-misc/r-2", .slot = "2"},
        {.cpv = "app-misc/p-1", .slot = "1"},
        {.cpv = "app-misc/p-2", .slot = "2"}};
    std::vector<std::string> world;
    for (const auto& [cpv, dep] : dependents) {
        installed.push_back({.cpv = cpv, .deps = {{"RDEPEND", dep}}});
        available.push_back({.cpv = cpv, .deps = {{"RDEPEND", dep}}});
        world.push_back(cpv.substr(0, cpv.rfind('-')));
    }
    const auto system = make_system(installed, available, world);
    const auto sorted = [](std::vector<std::string> lines) {
        std::ranges::sort(lines);
        return lines;
    };
    CHECK(sorted(plan(system, egraph::UseRebuilds::none,
                      egraph::Targets{.scope = {}, .roots = true, .deep = true})) ==
          std::vector<std::string>{
              "new app-misc/w-2 <- app-misc/plain-1 app-misc/w",
              "new app-misc/y-2 <- app-misc/older-1 app-misc/y",
              "new app-misc/z-2 <- app-misc/either-1 app-misc/z",
          });
    // @installed: every installed package is an argument, y-3 too.
    CHECK(sorted(plan(system)) == std::vector<std::string>{
                                      "new app-misc/w-2 <- app-misc/plain-1 app-misc/w",
                                      "new app-misc/z-2 <- app-misc/either-1 app-misc/z",
                                  });
    // Plain -u keeps a satisfied dependency.
    CHECK(plan(system, egraph::UseRebuilds::none,
               egraph::Targets{.scope = {}, .roots = true, .deep = false})
              .empty());
}

TEST_CASE(
    "an argument whose new slot is masked falls back to the version an installed slot holds") {
    const auto system = make_system(
        {{.cpv = "app-misc/v-1", .slot = "1"}},
        {{.cpv = "app-misc/v-1", .slot = "1"},
         {.cpv = "app-misc/v-2", .deps = {{"RDEPEND", "app-misc/missing"}}, .slot = "2"}});
    // Plain emerge reinstalls it, as it would had v-1 been the best version.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/v"})) ==
          std::vector<std::string>{"app-misc/v-1 -> app-misc/v-1"});
    auto update = reinstall({"app-misc/v"});
    update.selection = egraph::Selection::update;
    CHECK(plan(system, egraph::UseRebuilds::none, update).empty());
}

TEST_CASE("a merge new in its slot lists its cp's installed packages in their other slots") {
    const auto system = make_system(
        {{.cpv = "app-misc/slotty-1",
          .deps = {{"RDEPEND", "dev-lang/py:3.13 dev-lang/py:3.12 dev-libs/lib"}}},
         {.cpv = "dev-lang/py-3.13.1", .slot = "3.13"},
         {.cpv = "dev-lang/py-3.12.9", .slot = "3.12"},
         {.cpv = "dev-libs/lib-1"}},
        {{.cpv = "app-misc/slotty-1",
          .deps = {{"RDEPEND", "dev-lang/py:3.13 dev-lang/py:3.12 dev-libs/lib"}}},
         {.cpv = "app-misc/slotty-2",
          .deps = {{"RDEPEND", "dev-lang/py:3.14 dev-lang/py:3.12 dev-libs/lib dev-libs/fresh"}}},
         {.cpv = "dev-lang/py-3.12.9", .slot = "3.12"},
         {.cpv = "dev-lang/py-3.13.1", .slot = "3.13"},
         {.cpv = "dev-lang/py-3.14.1", .slot = "3.14"},
         {.cpv = "dev-libs/lib-1"},
         {.cpv = "dev-libs/lib-2"},
         {.cpv = "dev-libs/fresh-1"}});
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none);
    std::vector<std::string> lines;
    for (const auto& merge : found.merges) {
        auto line = std::string{evaluated.string(evaluated.candidates.at(merge.candidate).cpv)};
        for (const auto id : egraph::other_slots(store, evaluated, merge)) {
            line += std::format(" {}", store.string(store.packages.at(id).cpv));
        }
        lines.push_back(std::move(line));
    }
    std::ranges::sort(lines);
    // A replacement has none, nor has a package whose cp nothing installed shares.
    CHECK(lines == std::vector<std::string>{"app-misc/slotty-2",
                                            "dev-lang/py-3.14.1 "
                                            "dev-lang/py-3.12.9 dev-lang/py-3.13.1",
                                            "dev-libs/fresh-1", "dev-libs/lib-2"});
}

TEST_CASE("a merge new in its slot is a new-slot line, with the installed ones last") {
    const auto system = make_system(
        {{.cpv = "app-misc/slotty-1", .deps = {{"RDEPEND", "dev-lang/py:3.13"}}},
         {.cpv = "dev-lang/py-3.13.1", .slot = "3.13"}},
        {{.cpv = "app-misc/slotty-1", .deps = {{"RDEPEND", "dev-lang/py:3.13"}}},
         {.cpv = "app-misc/slotty-2", .deps = {{"RDEPEND", "dev-lang/py:3.14 dev-libs/fresh"}}},
         {.cpv = "dev-lang/py-3.13.1", .slot = "3.13"},
         {.cpv = "dev-lang/py-3.14.1", .slot = "3.14"},
         {.cpv = "dev-libs/fresh-1"}});
    CHECK(egraph::update_lines(system.store, system.evaluated, egraph::UseRebuilds::none) ==
          std::vector<std::string>{
              "app-misc/slotty-1\tupgrade\tapp-misc/slotty-2\ttest_repo",
              "dev-lang/py-3.14.1\tnew-slot\tdev-lang/py-3.14.1\ttest_repo\t\tapp-misc/slotty-2 "
              "dev-lang/py:3.14\tdev-lang/py-3.13.1:3.13",
              "dev-libs/fresh-1\tnew\tdev-libs/fresh-1\ttest_repo\t\tapp-misc/slotty-2 "
              "dev-libs/fresh"});
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
    CHECK(plan(system, egraph::UseRebuilds::none, {.scope = {false}}).empty());
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

TEST_CASE("a || keeps its installed alternative unless a root atom names it") {
    const std::vector<Installed> installed{
        {.cpv = "app-misc/either-1",
         .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
        {.cpv = "dev-libs/alt-1"}};
    const std::vector<Available> available{
        {.cpv = "app-misc/either-1",
         .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
        {.cpv = "dev-libs/alt-1"},
        {.cpv = "dev-libs/alt-2"},
        {.cpv = "dev-libs/other-1"}};
    CHECK(plan(make_system(installed, available, {"app-misc/either"}), egraph::UseRebuilds::none,
               {.scope = {}, .roots = true}) ==
          std::vector<std::string>{"dev-libs/alt-1 held <- app-misc/either-1 <dev-libs/alt-2"});
    CHECK(plan(make_system(installed, available, {"app-misc/either", "dev-libs/alt"}),
               egraph::UseRebuilds::none, {.scope = {}, .roots = true}) ==
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
    CHECK(plan(system, egraph::UseRebuilds::none, {.scope = {false, true}}) ==
          std::vector<std::string>{"dev-libs/astroid-4.0.4 -> dev-libs/astroid-4.3.2"});
}

TEST_CASE("a package out of scope keeps its version") {
    const auto system =
        make_system({{.cpv = "app-misc/tool-1", .deps = {{"RDEPEND", "dev-libs/lib"}}},
                     {.cpv = "dev-libs/lib-1"},
                     {.cpv = "app-misc/stray-1"}},
                    {{.cpv = "app-misc/tool-1", .deps = {{"RDEPEND", "dev-libs/lib"}}},
                     {.cpv = "dev-libs/lib-1"},
                     {.cpv = "dev-libs/lib-2"},
                     {.cpv = "app-misc/stray-1"},
                     {.cpv = "app-misc/stray-2"}});
    CHECK(plan(system) == std::vector<std::string>{"dev-libs/lib-1 -> dev-libs/lib-2",
                                                   "app-misc/stray-1 -> app-misc/stray-2"});
    CHECK(plan(system, egraph::UseRebuilds::none, {.scope = {true, true, false}}) ==
          std::vector<std::string>{"dev-libs/lib-1 -> dev-libs/lib-2"});
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
    CHECK(plan(system, egraph::UseRebuilds::none, {.scope = {false, true, true, true, true}}) ==
          std::vector<std::string>{
              "app-misc/ddep-1 -> app-misc/ddep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "app-misc/pdep-1 -> app-misc/pdep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "dev-libs/lib-1 -> dev-libs/lib-2"});
}

TEST_CASE("from the root sets, a slot-operator dependent moves to a newer slot") {
    const std::vector<Installed> installed{
        {.cpv = "app-misc/slotop-1", .deps = {{"RDEPEND", "dev-libs/lib:1/1="}}},
        {.cpv = "dev-libs/lib-1", .slot = "1", .sub_slot = "1"},
        {.cpv = "dev-libs/lib-2", .slot = "2", .sub_slot = "2"}};
    const std::vector<Available> available{
        {.cpv = "app-misc/slotop-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
        {.cpv = "dev-libs/lib-1", .slot = "1", .sub_slot = "1"},
        {.cpv = "dev-libs/lib-2", .slot = "2", .sub_slot = "2"},
        {.cpv = "dev-libs/lib-2.1", .slot = "2", .sub_slot = "2.1"}};
    const egraph::Targets world{.scope = {true, true, false}, .roots = true};
    // The installed package in the newer slot is updated though nothing kept it.
    CHECK(plan(make_system(installed, available, {"app-misc/slotop"}), egraph::UseRebuilds::none,
               world) ==
          std::vector<std::string>{
              "app-misc/slotop-1 -> app-misc/slotop-1 for dev-libs/lib-2.1 dev-libs/lib:1/1=",
              "dev-libs/lib-2 -> dev-libs/lib-2.1"});
    // Every installed package is an argument of @installed, pinning its slot.
    CHECK(plan(make_system(installed, available, {"app-misc/slotop"})) ==
          std::vector<std::string>{"dev-libs/lib-2 -> dev-libs/lib-2.1"});
    // So is a root atom's.
    CHECK(plan(make_system(installed, available, {"app-misc/slotop", "dev-libs/lib:1"}),
               egraph::UseRebuilds::none, world)
              .empty());
}

TEST_CASE("a newer slot nothing occupies is pulled in for a slot-operator dependent") {
    const auto system =
        make_system({{.cpv = "app-misc/slotop-1", .deps = {{"RDEPEND", "dev-libs/lib:1/1="}}},
                     {.cpv = "dev-libs/lib-1", .slot = "1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/slotop-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "dev-libs/lib-1", .slot = "1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .slot = "2", .sub_slot = "2"},
                     {.cpv = "dev-libs/lib-3", .slot = "3", .sub_slot = "3", .visible = false}},
                    {"app-misc/slotop"});
    CHECK(plan(system, egraph::UseRebuilds::none, {.scope = {}, .roots = true}) ==
          std::vector<std::string>{
              "app-misc/slotop-1 -> app-misc/slotop-1 for dev-libs/lib-2 dev-libs/lib:1/1=",
              "new dev-libs/lib-2 <- app-misc/slotop-1 dev-libs/lib:="});
}

TEST_CASE("a slot-operator dependent stays in its slot when anything else pins it there") {
    // Each other dependent with its installed dependencies, then its ebuild's.
    using Other = std::pair<Installed, egraph::test::DepStrings>;
    const auto with = [](const std::string& ebuild_atom, const std::vector<Other>& others) {
        std::vector<Installed> installed{
            {.cpv = "app-misc/slotop-1", .deps = {{"RDEPEND", "dev-libs/lib:1/1="}}},
            {.cpv = "dev-libs/lib-1", .slot = "1", .sub_slot = "1"}};
        std::vector<Available> available{
            {.cpv = "app-misc/slotop-1", .deps = {{"RDEPEND", ebuild_atom}}},
            {.cpv = "dev-libs/lib-1", .slot = "1", .sub_slot = "1"},
            {.cpv = "dev-libs/lib-2", .slot = "2", .sub_slot = "2"}};
        std::vector<std::string> world{"app-misc/slotop"};
        for (const auto& [other, deps] : others) {
            installed.push_back(other);
            available.push_back({.cpv = other.cpv, .deps = deps});
            world.push_back(other.cpv.substr(0, other.cpv.rfind('-')));
        }
        return make_system(installed, available, world);
    };
    // The ebuild names the slot.
    CHECK(plan(with("dev-libs/lib:1=", {}), egraph::UseRebuilds::none, {.scope = {}, .roots = true})
              .empty());
    // Another dependent's atom rejects the newer slot; another bound one does not.
    CHECK(plan(with("dev-libs/lib:=",
                    {{{.cpv = "app-misc/old-1", .deps = {{"RDEPEND", "<dev-libs/lib-2"}}},
                      {{"RDEPEND", "<dev-libs/lib-2"}}}}),
               egraph::UseRebuilds::none, {.scope = {}, .roots = true})
              .empty());
    CHECK(plan(with("dev-libs/lib:=",
                    {{{.cpv = "app-misc/bound-1", .deps = {{"RDEPEND", "dev-libs/lib:1/1="}}},
                      {{"RDEPEND", "dev-libs/lib:="}}}}),
               egraph::UseRebuilds::none, {.scope = {}, .roots = true}) ==
          std::vector<std::string>{
              "app-misc/slotop-1 -> app-misc/slotop-1 for dev-libs/lib-2 dev-libs/lib:1/1=",
              "app-misc/bound-1 -> app-misc/bound-1 for dev-libs/lib-2 dev-libs/lib:1/1=",
              "new dev-libs/lib-2 <- app-misc/slotop-1 dev-libs/lib:="});
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

TEST_CASE("-uD rebuilds a dependent bound to a sub-slot no longer installed") {
    const auto system = make_system(
        {{.cpv = "app-misc/stale-1", .deps = {{"RDEPEND", ">=dev-libs/utf-1:0/1="}}},
         {.cpv = "app-misc/built-1", .deps = {{"DEPEND", "dev-libs/old:0/1="}}},
         {.cpv = "app-misc/gone-1", .deps = {{"RDEPEND", "dev-libs/old:0/1="}}},
         {.cpv = "dev-libs/utf-2", .sub_slot = "2"},
         {.cpv = "dev-libs/old-2", .sub_slot = "2"}},
        {{.cpv = "app-misc/stale-1", .deps = {{"RDEPEND", ">=dev-libs/utf-1:="}}},
         {.cpv = "app-misc/built-1", .deps = {{"DEPEND", "dev-libs/old:="}}},
         {.cpv = "app-misc/gone-1", .deps = {{"RDEPEND", "dev-libs/old:="}}, .visible = false},
         {.cpv = "dev-libs/utf-1", .sub_slot = "1", .visible = false},
         {.cpv = "dev-libs/utf-2", .sub_slot = "2"},
         {.cpv = "dev-libs/old-1", .sub_slot = "1"},
         {.cpv = "dev-libs/old-2", .sub_slot = "2"}});
    // Against the installed package in the slot, for a build-time binding too. With no ebuild
    // to rebuild from, the binding is unsatisfied, as for every argument of @installed.
    CHECK(plan(system) ==
          std::vector<std::string>{
              "app-misc/stale-1 -> app-misc/stale-1 for dev-libs/utf-2 >=dev-libs/utf-1:0/1=",
              "app-misc/built-1 -> app-misc/built-1 for dev-libs/old-2 dev-libs/old:0/1=",
              "unsatisfied app-misc/gone-1 dev-libs/old:0/1="});
    // Plain -u leaves them.
    egraph::Targets shallow;
    shallow.deep = false;
    CHECK(plan(system, egraph::UseRebuilds::none, shallow).empty());
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

namespace {

// Merge targets in plan order, each with what it waits for and the kinds:
// "cpv <- cpv b, cpv r".
std::vector<std::string> ordered(const egraph::test::System& system) {
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none);
    const auto target = [&](std::uint32_t merge) {
        return std::string(
            evaluated.string(evaluated.candidates.at(found.merges.at(merge).candidate).cpv));
    };
    std::vector<std::string> lines;
    for (const auto merge : found.order) {
        auto line = target(merge);
        const auto& waits = found.merges.at(merge).waits;
        for (std::size_t i = 0; i < waits.size(); ++i) {
            line += std::format("{}{} {}", i == 0 ? " <- " : ", ", target(waits.at(i).merge),
                                egraph::wait_letters(waits.at(i).kinds));
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

TEST_CASE("merges come after what they wait for") {
    const auto system =
        make_system({{.cpv = "app-misc/glibmm-1"}},
                    {{.cpv = "app-misc/glibmm-1"},
                     {.cpv = "app-misc/glibmm-2", .deps = {{"BDEPEND", "dev-cpp/mm-common"}}},
                     {.cpv = "dev-cpp/mm-common-1", .deps = {{"RDEPEND", "dev-libs/chain"}}},
                     {.cpv = "dev-libs/chain-1"}});
    CHECK(ordered(system) == std::vector<std::string>{
                                 "dev-libs/chain-1",
                                 "dev-cpp/mm-common-1 <- dev-libs/chain-1 r",
                                 "app-misc/glibmm-2 <- dev-cpp/mm-common-1 b",
                             });
}

TEST_CASE("each wait carries the kinds of the dependencies behind it") {
    const auto system = make_system({{.cpv = "app-misc/top-1"},
                                     {.cpv = "dev-libs/b-1"},
                                     {.cpv = "dev-libs/d-1"},
                                     {.cpv = "dev-libs/i-1"},
                                     {.cpv = "dev-libs/r-1"}},
                                    {{.cpv = "app-misc/top-1"},
                                     {.cpv = "app-misc/top-2",
                                      .deps = {{"BDEPEND", "dev-libs/b"},
                                               {"DEPEND", "dev-libs/d dev-libs/r"},
                                               {"IDEPEND", "dev-libs/i"},
                                               {"RDEPEND", "dev-libs/r"}}},
                                     {.cpv = "dev-libs/b-1"},
                                     {.cpv = "dev-libs/b-2"},
                                     {.cpv = "dev-libs/d-1"},
                                     {.cpv = "dev-libs/d-2"},
                                     {.cpv = "dev-libs/i-1"},
                                     {.cpv = "dev-libs/i-2"},
                                     {.cpv = "dev-libs/r-1"},
                                     {.cpv = "dev-libs/r-2"}});
    CHECK(ordered(system) ==
          std::vector<std::string>{"dev-libs/b-2", "dev-libs/d-2", "dev-libs/i-2", "dev-libs/r-2",
                                   "app-misc/top-2 <- dev-libs/b-2 b, dev-libs/d-2 b, "
                                   "dev-libs/i-2 i, dev-libs/r-2 br"});
}

TEST_CASE("a merge waits for what the installed packages it depends on lead to, after it where "
          "it can") {
    // a reaches lib through x and y, which stay; nothing reaches through z, which is replaced.
    const auto system =
        make_system({{.cpv = "app-misc/a-1"},
                     {.cpv = "app-misc/x-1", .deps = {{"RDEPEND", "app-misc/y"}}},
                     {.cpv = "app-misc/y-1", .deps = {{"DEPEND", "dev-libs/lib"}}},
                     {.cpv = "app-misc/z-1", .deps = {{"RDEPEND", "dev-libs/other"}}},
                     {.cpv = "dev-libs/lib-1"},
                     {.cpv = "dev-libs/other-1"}},
                    {{.cpv = "app-misc/a-1"},
                     {.cpv = "app-misc/a-2", .deps = {{"RDEPEND", "app-misc/x app-misc/z"}}},
                     {.cpv = "app-misc/x-1", .deps = {{"RDEPEND", "app-misc/y"}}},
                     {.cpv = "app-misc/y-1", .deps = {{"DEPEND", "dev-libs/lib"}}},
                     {.cpv = "app-misc/z-1", .deps = {{"RDEPEND", "dev-libs/other"}}},
                     {.cpv = "app-misc/z-2"},
                     {.cpv = "dev-libs/lib-1"},
                     {.cpv = "dev-libs/lib-2"},
                     {.cpv = "dev-libs/other-1"},
                     {.cpv = "dev-libs/other-2"}});
    CHECK(ordered(system) ==
          std::vector<std::string>{"app-misc/z-2", "dev-libs/lib-2",
                                   "app-misc/a-2 <- app-misc/z-2 r, dev-libs/lib-2 t",
                                   "dev-libs/other-2"});
}

TEST_CASE("PDEPEND holds a merge back, as emerge's order, until a cycle drops it first") {
    const auto system = make_system({{.cpv = "app-misc/x-1"}, {.cpv = "app-misc/y-1"}},
                                    {{.cpv = "app-misc/x-1"},
                                     {.cpv = "app-misc/x-2", .deps = {{"PDEPEND", "app-misc/y"}}},
                                     {.cpv = "app-misc/y-1"},
                                     {.cpv = "app-misc/y-2"}});
    CHECK(ordered(system) ==
          std::vector<std::string>{"app-misc/y-2", "app-misc/x-2 <- app-misc/y-2 p"});
    // a needs b to run and b's PDEPEND is a: the PDEPEND goes first, though a comes first.
    const auto cycle = make_system({{.cpv = "app-misc/a-1"}, {.cpv = "app-misc/b-1"}},
                                   {{.cpv = "app-misc/a-1"},
                                    {.cpv = "app-misc/a-2", .deps = {{"RDEPEND", "app-misc/b"}}},
                                    {.cpv = "app-misc/b-1"},
                                    {.cpv = "app-misc/b-2", .deps = {{"PDEPEND", "app-misc/a"}}}});
    CHECK(ordered(cycle) == std::vector<std::string>{"app-misc/b-2 <- app-misc/a-2 p",
                                                     "app-misc/a-2 <- app-misc/b-2 r"});
}

TEST_CASE("a slot-operator rebuild waits for the merge it is for") {
    const auto system =
        make_system({{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .sub_slot = "2"}});
    CHECK(ordered(system) ==
          std::vector<std::string>{"dev-libs/lib-2", "app-misc/rdep-1 <- dev-libs/lib-2 r"});
}

TEST_CASE("every alternative of a || that merges is waited for") {
    const auto system =
        make_system({{.cpv = "app-misc/c-1"}, {.cpv = "app-misc/d-1"}, {.cpv = "app-misc/e-1"}},
                    {{.cpv = "app-misc/c-1"},
                     {.cpv = "app-misc/c-2", .deps = {{"RDEPEND", "|| ( app-misc/d app-misc/e )"}}},
                     {.cpv = "app-misc/d-1"},
                     {.cpv = "app-misc/d-2"},
                     {.cpv = "app-misc/e-1"},
                     {.cpv = "app-misc/e-2"}});
    CHECK(ordered(system) ==
          std::vector<std::string>{"app-misc/d-2", "app-misc/e-2",
                                   "app-misc/c-2 <- app-misc/d-2 r, app-misc/e-2 r"});
}

TEST_CASE("a cycle drops a run-time wait before a build-time one") {
    // b needs a to build, a needs b to run: a goes first.
    const auto system = make_system({{.cpv = "app-misc/b-1"}, {.cpv = "app-misc/a-1"}},
                                    {{.cpv = "app-misc/a-1"},
                                     {.cpv = "app-misc/a-2", .deps = {{"RDEPEND", "app-misc/b"}}},
                                     {.cpv = "app-misc/b-1"},
                                     {.cpv = "app-misc/b-2", .deps = {{"BDEPEND", "app-misc/a"}}}});
    CHECK(ordered(system) == std::vector<std::string>{"app-misc/a-2 <- app-misc/b-2 r",
                                                      "app-misc/b-2 <- app-misc/a-2 b"});
    // With only build-time waits, one of them has to go.
    const auto both = make_system({{.cpv = "app-misc/p-1"}, {.cpv = "app-misc/q-1"}},
                                  {{.cpv = "app-misc/p-1"},
                                   {.cpv = "app-misc/p-2", .deps = {{"DEPEND", "app-misc/q"}}},
                                   {.cpv = "app-misc/q-1"},
                                   {.cpv = "app-misc/q-2", .deps = {{"DEPEND", "app-misc/p"}}}});
    CHECK(ordered(both) == std::vector<std::string>{"app-misc/p-2 <- app-misc/q-2 b",
                                                    "app-misc/q-2 <- app-misc/p-2 b"});
}

TEST_CASE("the table lists merges in order, each with the places it waits for") {
    const auto system =
        make_system({{.cpv = "app-misc/glibmm-1"},
                     {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<app-misc/host-2"}}},
                     {.cpv = "app-misc/host-1"}},
                    {{.cpv = "app-misc/glibmm-1"},
                     {.cpv = "app-misc/glibmm-2", .deps = {{"BDEPEND", "dev-cpp/mm-common"}}},
                     {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<app-misc/host-2"}}},
                     {.cpv = "app-misc/host-1"},
                     {.cpv = "app-misc/host-2"},
                     {.cpv = "dev-cpp/mm-common-1", .deps = {{"RDEPEND", "dev-libs/chain"}}},
                     {.cpv = "dev-libs/chain-1"}});
    CHECK(
        egraph::update_lines(system.store, system.evaluated, egraph::UseRebuilds::none,
                             egraph::HeldLines::all, true) ==
        std::vector<std::string>{
            "1\t\tdev-libs/chain-1\tnew\tdev-libs/chain-1\ttest_repo\t\tdev-cpp/mm-common-1 "
            "dev-libs/chain",
            "2\t1r\tdev-cpp/mm-common-1\tnew\tdev-cpp/mm-common-1\ttest_repo\t\tapp-misc/glibmm-2 "
            "dev-cpp/mm-common",
            "3\t2b\tapp-misc/glibmm-1\tupgrade\tapp-misc/glibmm-2\ttest_repo",
            "\t\tapp-misc/host-1\theld\tapp-misc/host-2\ttest_repo\t\tapp-misc/holder-1 "
            "<app-misc/host-2"});
}

TEST_CASE("libc and what it waits for merge first, as emerge's implicit libc dependency") {
    const auto system =
        make_system({{.cpv = "app-misc/a-1"},
                     {.cpv = "virtual/libc-1", .deps = {{"RDEPEND", "sys-libs/glibc"}}},
                     {.cpv = "sys-libs/glibc-1"},
                     {.cpv = "sys-kernel/headers-1"}},
                    {{.cpv = "app-misc/a-1"},
                     {.cpv = "app-misc/a-2"},
                     {.cpv = "virtual/libc-1", .deps = {{"RDEPEND", "sys-libs/glibc"}}},
                     {.cpv = "sys-libs/glibc-1"},
                     {.cpv = "sys-libs/glibc-2", .deps = {{"DEPEND", "sys-kernel/headers"}}},
                     {.cpv = "sys-kernel/headers-1"},
                     {.cpv = "sys-kernel/headers-2"}});
    CHECK(ordered(system) == std::vector<std::string>{"sys-kernel/headers-2",
                                                      "sys-libs/glibc-2 <- sys-kernel/headers-2 b",
                                                      "app-misc/a-2 <- sys-libs/glibc-2 l"});
}

TEST_CASE("a rebuild of the installed libc is no implicit wait, as emerge's") {
    const auto system =
        make_system({{.cpv = "app-misc/a-1"},
                     {.cpv = "virtual/libc-1", .deps = {{"RDEPEND", "sys-libs/glibc"}}},
                     {.cpv = "sys-libs/glibc-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"}},
                    {{.cpv = "app-misc/a-1"},
                     {.cpv = "app-misc/a-2"},
                     {.cpv = "virtual/libc-1", .deps = {{"RDEPEND", "sys-libs/glibc"}}},
                     {.cpv = "sys-libs/glibc-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "dev-libs/lib-1", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .sub_slot = "2"}});
    CHECK(ordered(system) == std::vector<std::string>{"dev-libs/lib-2",
                                                      "sys-libs/glibc-1 <- dev-libs/lib-2 r",
                                                      "app-misc/a-2"});
}

TEST_CASE("the tree leads each merge down from its root, a new package through its puller") {
    const auto system =
        make_system({{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}},
                     {.cpv = "app-misc/glibmm-1"},
                     {.cpv = "app-misc/loose-1"}},
                    {{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}},
                     {.cpv = "app-misc/glibmm-1"},
                     {.cpv = "app-misc/glibmm-2", .deps = {{"BDEPEND", "dev-cpp/mm-common"}}},
                     {.cpv = "app-misc/loose-1"},
                     {.cpv = "app-misc/loose-2"},
                     {.cpv = "dev-cpp/mm-common-1", .deps = {{"RDEPEND", "dev-libs/chain"}}},
                     {.cpv = "dev-libs/chain-1"}},
                    {"app-misc/top"});
    const auto kept = egraph::keep(system.store, {});
    CHECK(egraph::update_tree_lines(system.store, system.evaluated, kept,
                                    egraph::UseRebuilds::none) ==
          std::vector<std::string>{
              "1\t\tapp-misc/loose-1",
              "2\t@selected\tapp-misc/top-1\tapp-misc/glibmm-1\tdev-cpp/mm-common-1\tdev-libs/"
              "chain-1",
              "3\t@selected\tapp-misc/top-1\tapp-misc/glibmm-1\tdev-cpp/mm-common-1",
              "4\t@selected\tapp-misc/top-1\tapp-misc/glibmm-1"});
}

namespace {

constexpr egraph::Targets shallow{.scope = {}, .roots = false, .deep = false};
constexpr egraph::Targets shallow_world{.scope = {}, .roots = true, .deep = false};

} // namespace

TEST_CASE("without --deep, only the arguments are updated, and what their merges need") {
    const auto system =
        make_system({{.cpv = "app-misc/needs-1"},
                     {.cpv = "dev-libs/base-1"},
                     {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "dev-libs/idle"}}},
                     {.cpv = "dev-libs/idle-1"}},
                    {{.cpv = "app-misc/needs-1"},
                     {.cpv = "app-misc/needs-2", .deps = {{"RDEPEND", ">=dev-libs/base-2"}}},
                     {.cpv = "dev-libs/base-1"},
                     {.cpv = "dev-libs/base-2"},
                     {.cpv = "dev-libs/base-3"},
                     {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "dev-libs/idle"}}},
                     {.cpv = "dev-libs/idle-1"},
                     {.cpv = "dev-libs/idle-2"}},
                    {"app-misc/needs", "app-misc/user"});
    CHECK(plan(system, egraph::UseRebuilds::none, shallow_world) ==
          std::vector<std::string>{"app-misc/needs-1 -> app-misc/needs-2",
                                   "dev-libs/base-1 -> dev-libs/base-3"});
    // Every installed package is an argument of @installed.
    CHECK(plan(system, egraph::UseRebuilds::none, shallow) ==
          std::vector<std::string>{"app-misc/needs-1 -> app-misc/needs-2",
                                   "dev-libs/base-1 -> dev-libs/base-3",
                                   "dev-libs/idle-1 -> dev-libs/idle-2"});
}

TEST_CASE("without --deep, an update a dependent rejects is dropped, not fallen back") {
    const auto system =
        make_system({{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
                     {.cpv = "dev-libs/astroid-4.0.4"},
                     {.cpv = "app-misc/either-1",
                      .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
                     {.cpv = "dev-libs/alt-1"}},
                    {{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
                     {.cpv = "dev-libs/astroid-4.0.4"},
                     {.cpv = "dev-libs/astroid-4.0.5"},
                     {.cpv = "dev-libs/astroid-4.3.2"},
                     {.cpv = "app-misc/either-1",
                      .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
                     {.cpv = "dev-libs/alt-1"},
                     {.cpv = "dev-libs/alt-2"},
                     {.cpv = "dev-libs/other-1"}});
    CHECK(plan(system, egraph::UseRebuilds::none, shallow) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0.4 held <- app-misc/pylint-1 <dev-libs/astroid-4.1",
              "dev-libs/alt-1 held <- app-misc/either-1 <dev-libs/alt-2"});
    CHECK(plan(system) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0.4 -> dev-libs/astroid-4.0.5",
              "dev-libs/alt-1 -> dev-libs/alt-2",
              "new dev-libs/other-1 <- app-misc/either-1 dev-libs/other",
              "dev-libs/astroid-4.0.4 held <- app-misc/pylint-1 <dev-libs/astroid-4.1"});
}

TEST_CASE("without --deep, a sub-slot change is dropped rather than rebuild what binds to it") {
    const auto system =
        make_system({{.cpv = "dev-libs/solib-1", .slot = "0", .sub_slot = "1"},
                     {.cpv = "app-misc/consumer-1", .deps = {{"RDEPEND", "dev-libs/solib:0/1="}}}},
                    {{.cpv = "dev-libs/solib-1", .slot = "0", .sub_slot = "1"},
                     {.cpv = "dev-libs/solib-2", .slot = "0", .sub_slot = "2"},
                     {.cpv = "app-misc/consumer-1", .deps = {{"RDEPEND", "dev-libs/solib:="}}}});
    CHECK(plan(system, egraph::UseRebuilds::none, shallow) ==
          std::vector<std::string>{
              "dev-libs/solib-1 held <- app-misc/consumer-1 dev-libs/solib:0/1="});
}

TEST_CASE("without --deep, a kept package's missing dependencies pull nothing in") {
    const auto system =
        make_system({{.cpv = "app-misc/grown-1", .deps = {{"RDEPEND", "dev-libs/fresh"}}}},
                    {{.cpv = "app-misc/grown-1", .deps = {{"RDEPEND", "dev-libs/fresh"}}},
                     {.cpv = "dev-libs/fresh-1"}});
    CHECK(plan(system, egraph::UseRebuilds::none, shallow).empty());
    CHECK(plan(system) ==
          std::vector<std::string>{"new dev-libs/fresh-1 <- app-misc/grown-1 dev-libs/fresh"});
}

TEST_CASE("without --deep, an update needing one another root holds back is dropped") {
    const auto system =
        make_system({{.cpv = "app-misc/wants-1"},
                     {.cpv = "dev-libs/mid-1"},
                     {.cpv = "app-misc/caps-1", .deps = {{"RDEPEND", "<dev-libs/mid-2"}}}},
                    {{.cpv = "app-misc/wants-1"},
                     {.cpv = "app-misc/wants-2", .deps = {{"RDEPEND", ">=dev-libs/mid-2"}}},
                     {.cpv = "dev-libs/mid-1"},
                     {.cpv = "dev-libs/mid-2"},
                     {.cpv = "app-misc/caps-1", .deps = {{"RDEPEND", "<dev-libs/mid-2"}}}},
                    {"app-misc/wants", "app-misc/caps"});
    // The update wants-2 needs is no argument's, so it is not listed as held itself.
    CHECK(plan(system, egraph::UseRebuilds::none, shallow_world) ==
          std::vector<std::string>{"app-misc/wants-1 held <- app-misc/wants-2 >=dev-libs/mid-2"});
}

TEST_CASE("a merge needing a newer version of a package nothing keeps replaces it") {
    // base is outside the scope depclean keeps, so it has no update of its own.
    const auto system =
        make_system({{.cpv = "app-misc/needs-1"}, {.cpv = "dev-libs/base-1"}},
                    {{.cpv = "app-misc/needs-1"},
                     {.cpv = "app-misc/needs-2", .deps = {{"RDEPEND", ">=dev-libs/base-2"}}},
                     {.cpv = "dev-libs/base-1"},
                     {.cpv = "dev-libs/base-2"},
                     {.cpv = "dev-libs/base-3"}},
                    {"app-misc/needs"});
    const egraph::Targets world{.scope = {true, false}, .roots = true};
    const std::vector<std::string> expected{"app-misc/needs-1 -> app-misc/needs-2",
                                            "dev-libs/base-1 -> dev-libs/base-3"};
    CHECK(plan(system, egraph::UseRebuilds::none, world) == expected);
    CHECK(plan(system, egraph::UseRebuilds::none,
               {.scope = {true, false}, .roots = true, .deep = false}) == expected);
}

TEST_CASE("a needed version older than the installed one is no replacement") {
    const auto system =
        make_system({{.cpv = "app-misc/old-1"}, {.cpv = "dev-libs/base-2"}},
                    {{.cpv = "app-misc/old-1"},
                     {.cpv = "app-misc/old-2", .deps = {{"RDEPEND", "<dev-libs/base-2"}}},
                     {.cpv = "dev-libs/base-1"},
                     {.cpv = "dev-libs/base-2"}},
                    {"app-misc/old"});
    CHECK(plan(system, egraph::UseRebuilds::none, shallow_world) ==
          std::vector<std::string>{"app-misc/old-1 held <- app-misc/old-2 <dev-libs/base-2"});
}

TEST_CASE("a root atom's best version in a slot nothing occupies is pulled in") {
    const auto system = make_system({{.cpv = "dev-lang/lang-1", .slot = "1"}},
                                    {{.cpv = "dev-lang/lang-1", .slot = "1"},
                                     {.cpv = "dev-lang/lang-2", .slot = "2"},
                                     {.cpv = "dev-lang/lang-3", .slot = "3", .visible = false}},
                                    {"dev-lang/lang"});
    const std::vector<std::string> expected{"new dev-lang/lang-2 <- "};
    CHECK(plan(system, egraph::UseRebuilds::none, {.scope = {}, .roots = true}) == expected);
    CHECK(plan(system, egraph::UseRebuilds::none, shallow_world) == expected);
    // @installed's slot atoms name only the installed slot.
    CHECK(plan(system, egraph::UseRebuilds::none, shallow).empty());
    // The line names the root atom.
    CHECK(egraph::update_lines(system.store, system.evaluated, egraph::UseRebuilds::none,
                               egraph::HeldLines::none, false, {.scope = {}, .roots = true}) ==
          std::vector<std::string>{
              "dev-lang/lang-2\tnew-slot\tdev-lang/lang-2\ttest_repo\t\t@selected dev-lang/lang"
              "\tdev-lang/lang-1:1"});
}

namespace {

egraph::Targets request(std::vector<std::string> atoms,
                        egraph::Selection selection = egraph::Selection::update,
                        bool deep = false) {
    std::vector<egraph::Argument> arguments;
    for (auto& atom : atoms) {
        arguments.push_back({.set = "", .atom = std::move(atom)});
    }
    return {.scope = {},
            .roots = true,
            .deep = deep,
            .request = std::move(arguments),
            .selection = selection};
}

} // namespace

TEST_CASE("-u updates each installed slot a requested atom matches, and adds its best slot") {
    const auto system = make_system({{.cpv = "dev-lang/lang-1.0", .slot = "1"},
                                     {.cpv = "dev-lang/lang-2.0", .slot = "2"},
                                     {.cpv = "app-misc/other-1"}},
                                    {{.cpv = "dev-lang/lang-1.0", .slot = "1"},
                                     {.cpv = "dev-lang/lang-1.1", .slot = "1"},
                                     {.cpv = "dev-lang/lang-2.0", .slot = "2"},
                                     {.cpv = "dev-lang/lang-2.1", .slot = "2"},
                                     {.cpv = "dev-lang/lang-3.0", .slot = "3"},
                                     {.cpv = "app-misc/other-1"},
                                     {.cpv = "app-misc/other-2"}});
    CHECK(plan(system, egraph::UseRebuilds::none, request({"dev-lang/lang"})) ==
          std::vector<std::string>{"dev-lang/lang-1.0 -> dev-lang/lang-1.1",
                                   "dev-lang/lang-2.0 -> dev-lang/lang-2.1",
                                   "new dev-lang/lang-3.0 <- "});
}

TEST_CASE("-u takes a requested atom's best version in its slot, whichever way it moves") {
    const auto system = make_system(
        {{.cpv = "app-misc/up-1"}, {.cpv = "app-misc/down-2"}, {.cpv = "app-misc/ahead-3"}},
        {{.cpv = "app-misc/up-1"},
         {.cpv = "app-misc/up-2"},
         {.cpv = "app-misc/up-3"},
         {.cpv = "app-misc/down-1"},
         {.cpv = "app-misc/down-2"},
         {.cpv = "app-misc/ahead-2"}});
    CHECK(plan(system, egraph::UseRebuilds::none, request({"<app-misc/up-3"})) ==
          std::vector<std::string>{"app-misc/up-1 -> app-misc/up-2"});
    CHECK(plan(system, egraph::UseRebuilds::none, request({"=app-misc/down-1"})) ==
          std::vector<std::string>{"app-misc/down-2 -> app-misc/down-1"});
    // emerge keeps no installed version without a visible ebuild when one in its slot is.
    CHECK(plan(system, egraph::UseRebuilds::none, request({"app-misc/ahead"})) ==
          std::vector<std::string>{"app-misc/ahead-3 -> app-misc/ahead-2"});
}

TEST_CASE("-uD falls back only to versions a set's atom matches, and never for one named alone") {
    const auto system = make_system(
        {{.cpv = "app-misc/up-1"},
         {.cpv = "app-misc/pins-1",
          .deps = {{"RDEPEND", "|| ( =app-misc/up-1 =app-misc/up-2 =app-misc/up-3 )"}}}},
        {{.cpv = "app-misc/up-1"},
         {.cpv = "app-misc/up-2"},
         {.cpv = "app-misc/up-2.5"},
         {.cpv = "app-misc/up-3"},
         {.cpv = "app-misc/pins-1",
          .deps = {{"RDEPEND", "|| ( =app-misc/up-1 =app-misc/up-2 =app-misc/up-3 )"}}}});
    auto in_set = request({"<app-misc/up-3"}, egraph::Selection::update, true);
    in_set.request.front().set = "selected";
    CHECK(plan(system, egraph::UseRebuilds::none, in_set) ==
          std::vector<std::string>{"app-misc/up-1 -> app-misc/up-2",
                                   "app-misc/up-1 held <- app-misc/pins-1 =app-misc/up-1"});
    // Named alone, emerge keeps the installed version instead.
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"<app-misc/up-3"}, egraph::Selection::update, true)) ==
          std::vector<std::string>{"app-misc/up-1 held <- app-misc/pins-1 =app-misc/up-1"});
}

TEST_CASE("plain emerge merges a requested atom's best version, installed or not") {
    const auto system = make_system({{.cpv = "app-misc/same-2"},
                                     {.cpv = "app-misc/older-1"},
                                     {.cpv = "dev-lang/lang-1", .slot = "1"}},
                                    {{.cpv = "app-misc/same-2"},
                                     {.cpv = "app-misc/older-1"},
                                     {.cpv = "app-misc/older-2"},
                                     {.cpv = "app-misc/fresh-1"},
                                     {.cpv = "dev-lang/lang-1", .slot = "1"},
                                     {.cpv = "dev-lang/lang-2", .slot = "2"}});
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"app-misc/same", "app-misc/older", "app-misc/fresh", "dev-lang/lang"},
                       egraph::Selection::reinstall)) ==
          std::vector<std::string>{"app-misc/same-2 -> app-misc/same-2",
                                   "app-misc/older-1 -> app-misc/older-2",
                                   "new app-misc/fresh-1 <- ", "new dev-lang/lang-2 <- "});
}

TEST_CASE("--noreplace merges a requested atom only when nothing installed matches it") {
    const auto system = make_system(
        {{.cpv = "app-misc/up-1"}},
        {{.cpv = "app-misc/up-1"}, {.cpv = "app-misc/up-2"}, {.cpv = "app-misc/fresh-1"}});
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"app-misc/up"}, egraph::Selection::noreplace))
              .empty());
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"=app-misc/up-2", "app-misc/fresh"}, egraph::Selection::noreplace)) ==
          std::vector<std::string>{"app-misc/up-1 -> app-misc/up-2", "new app-misc/fresh-1 <- "});
}

TEST_CASE("a new package a request names is listed with its argument") {
    const auto system = make_system({}, {{.cpv = "app-misc/fresh-1"}});
    CHECK(egraph::update_lines(system.store, system.evaluated, egraph::UseRebuilds::none,
                               egraph::HeldLines::none, false, request({"app-misc/fresh"})) ==
          std::vector<std::string>{
              "app-misc/fresh-1\tnew\tapp-misc/fresh-1\ttest_repo\t\tapp-misc/fresh"});
}

namespace {

egraph::test::System flagged_system() {
    auto system = make_system(
        {}, {{.cpv = "dev-libs/fresh-1",
              .iuse = "on off a10 a9 fixed stuck python_targets_py3_13 python_targets_py3_12 "
                      "python_targets_py3_11 video_cards_intel",
              .use = "on a10 a9 fixed python_targets_py3_13 python_targets_py3_12 "
                     "video_cards_intel elibc_glibc",
              .forced = "fixed python_targets_py3_12 stuck"}});
    egraph::test::set_use_expand(system, {"python_targets", "video_cards"}, {"video_cards"});
    return system;
}

} // namespace

TEST_CASE("a new package's USE is emerge's: groups, forced flags in parentheses, alnum order") {
    const auto system = flagged_system();
    CHECK(egraph::use_display(system.evaluated, system.evaluated.candidates.front()) ==
          R"x(USE="a9 a10 (fixed) on -off (-stuck)" PYTHON_TARGETS="(py3_12) py3_13 -py3_11")x");
}

TEST_CASE("a package without IUSE shows no USE") {
    const auto system = make_system({}, {{.cpv = "app-misc/fresh-1", .use = "elibc_glibc"}});
    CHECK(egraph::use_display(system.evaluated, system.evaluated.candidates.front()).empty());
}

TEST_CASE("a group of hidden flags alone shows nothing") {
    auto system = make_system(
        {}, {{.cpv = "app-misc/fresh-1", .iuse = "video_cards_intel", .use = "video_cards_intel"}});
    egraph::test::set_use_expand(system, {"video_cards"}, {"video_cards"});
    CHECK(egraph::use_display(system.evaluated, system.evaluated.candidates.front()).empty());
}

TEST_CASE("a new package's line carries its USE before why it comes in") {
    const auto system = flagged_system();
    CHECK(egraph::update_lines(system.store, system.evaluated, egraph::UseRebuilds::none,
                               egraph::HeldLines::none, false, request({"dev-libs/fresh"})) ==
          std::vector<std::string>{
              "dev-libs/fresh-1\tnew\tdev-libs/fresh-1\ttest_repo\t"
              R"x(USE="a9 a10 (fixed) on -off (-stuck)" PYTHON_TARGETS="(py3_12) py3_13 -py3_11")x"
              "\tdev-libs/fresh"});
}

TEST_CASE("flags sort as emerge's alnum key does: digit runs as numbers") {
    CHECK(egraph::alnum_less("a9", "a10"));
    CHECK_FALSE(egraph::alnum_less("a10", "a9"));
    CHECK(egraph::alnum_less("a", "a1"));
    CHECK(egraph::alnum_less("a1", "ab"));
    CHECK(egraph::alnum_less("1b", "a"));
    CHECK(egraph::alnum_less("py3_9", "py3_10"));
    CHECK(egraph::alnum_less("x99999999999999999999", "x100000000000000000000"));
    CHECK(egraph::alnum_less("a07", "a7"));
    CHECK_FALSE(egraph::alnum_less("a7", "a07"));
}

TEST_CASE("plain emerge falls back to another version it matches when the best is rejected") {
    const auto system =
        make_system({{.cpv = "app-misc/effects-1", .deps = {{"RDEPEND", "app-misc/rgb"}}},
                     {.cpv = "app-misc/rgb-1"},
                     {.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<app-misc/rgb-2"}}}},
                    {{.cpv = "app-misc/effects-1", .deps = {{"RDEPEND", "app-misc/rgb"}}},
                     {.cpv = "app-misc/effects-2", .deps = {{"RDEPEND", ">=app-misc/rgb-2"}}},
                     {.cpv = "app-misc/rgb-1"},
                     {.cpv = "app-misc/rgb-2"},
                     {.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<app-misc/rgb-2"}}}});
    CHECK(
        plan(system, egraph::UseRebuilds::none,
             request({"app-misc/effects"}, egraph::Selection::reinstall)) ==
        std::vector<std::string>{"app-misc/effects-1 -> app-misc/effects-1",
                                 "app-misc/effects-1 held <- app-misc/effects-2 >=app-misc/rgb-2"});
}

TEST_CASE("--noreplace replaces a masked installed match, not one whose ebuild is gone") {
    auto system = make_system({{.cpv = "app-misc/masked-2"}, {.cpv = "app-misc/gone-2"}},
                              {{.cpv = "app-misc/masked-1"},
                               {.cpv = "app-misc/masked-2", .visible = false},
                               {.cpv = "app-misc/gone-1"}});
    system.evaluated.packages.at(0).masked = true;
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"app-misc/masked", "app-misc/gone"}, egraph::Selection::noreplace)) ==
          std::vector<std::string>{"app-misc/masked-2 -> app-misc/masked-1"});
}

TEST_CASE("-u keeps an installed version only an atom's masked ebuild matches") {
    const auto system =
        make_system({{.cpv = "app-misc/down-2"}},
                    {{.cpv = "app-misc/down-1"}, {.cpv = "app-misc/down-2", .visible = false}});
    // Its pending update is the downgrade to down-1, which =down-2 does not accept.
    CHECK(plan(system, egraph::UseRebuilds::none, request({"=app-misc/down-2"})).empty());
    CHECK(plan(system, egraph::UseRebuilds::none, request({"app-misc/down"})) ==
          std::vector<std::string>{"app-misc/down-2 -> app-misc/down-1"});
}

TEST_CASE("-u rebuilds what binds to a version it must merge, as plain emerge does") {
    const auto system =
        make_system({{.cpv = "dev-libs/bound-0.3", .slot = "0", .sub_slot = "3"},
                     {.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "dev-libs/bound:0/3="}}}},
                    {{.cpv = "dev-libs/bound-0.3", .slot = "0", .sub_slot = "3"},
                     {.cpv = "dev-libs/bound-0.4", .slot = "0", .sub_slot = "4"},
                     {.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "dev-libs/bound:="}}}});
    // The installed version is no answer to =bound-0.4.
    CHECK(plan(system, egraph::UseRebuilds::none, request({"=dev-libs/bound-0.4"})) ==
          std::vector<std::string>{
              "dev-libs/bound-0.3 -> dev-libs/bound-0.4",
              "app-misc/kwin-1 -> app-misc/kwin-1 for dev-libs/bound-0.4 dev-libs/bound:0/3="});
    // It is to bound, which -u keeps rather than rebuild what binds to it.
    CHECK(
        plan(system, egraph::UseRebuilds::none, request({"dev-libs/bound"})) ==
        std::vector<std::string>{"dev-libs/bound-0.3 held <- app-misc/kwin-1 dev-libs/bound:0/3="});
}

TEST_CASE("-uD keeps an atom's installed version that a || prefers") {
    const auto system =
        make_system({{.cpv = "app-misc/either-1",
                      .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
                     {.cpv = "dev-libs/alt-1"}},
                    {{.cpv = "app-misc/either-1",
                      .deps = {{"RDEPEND", "|| ( <dev-libs/alt-2 dev-libs/other )"}}},
                     {.cpv = "dev-libs/alt-1"},
                     {.cpv = "dev-libs/alt-2"},
                     {.cpv = "dev-libs/other-1"}},
                    {"app-misc/either"});
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"dev-libs/alt"}, egraph::Selection::update, true)) ==
          std::vector<std::string>{"dev-libs/alt-1 held <- app-misc/either-1 <dev-libs/alt-2"});
}

TEST_CASE("-uD with a reach updates only it, and what lies outside it only weighs") {
    // grown lacks fresh, and idle has an update, but neither is the argument's.
    const auto system =
        make_system({{.cpv = "app-misc/arg-1", .deps = {{"RDEPEND", "dev-libs/dep"}}},
                     {.cpv = "dev-libs/dep-1"},
                     {.cpv = "app-misc/grown-1", .deps = {{"RDEPEND", "dev-libs/fresh"}}},
                     {.cpv = "dev-libs/idle-1"}},
                    {{.cpv = "app-misc/arg-1", .deps = {{"RDEPEND", "dev-libs/dep"}}},
                     {.cpv = "app-misc/arg-2", .deps = {{"RDEPEND", "dev-libs/dep dev-libs/new"}}},
                     {.cpv = "dev-libs/dep-1"},
                     {.cpv = "dev-libs/dep-2"},
                     {.cpv = "dev-libs/new-1"},
                     {.cpv = "app-misc/grown-1", .deps = {{"RDEPEND", "dev-libs/fresh"}}},
                     {.cpv = "dev-libs/fresh-1"},
                     {.cpv = "dev-libs/idle-1"},
                     {.cpv = "dev-libs/idle-2"}});
    auto targets = request({"app-misc/arg"}, egraph::Selection::update, true);
    targets.reach = {true, true, false, false};
    CHECK(plan(system, egraph::UseRebuilds::none, targets) ==
          std::vector<std::string>{"app-misc/arg-1 -> app-misc/arg-2",
                                   "dev-libs/dep-1 -> dev-libs/dep-2",
                                   "new dev-libs/new-1 <- app-misc/arg-2 dev-libs/new"});
}

TEST_CASE("-uD rebuilds outside its reach only for a merge that breaks a binding within it") {
    // Neither rdep nor other is the argument's, and the argument's own binding goes with its
    // update, so emerge's slot-operator backtracking never starts and lib's update is dropped.
    const auto system =
        make_system({{.cpv = "dev-libs/lib-1", .slot = "0", .sub_slot = "1"},
                     {.cpv = "app-misc/moving-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "app-misc/other-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}}},
                    {{.cpv = "dev-libs/lib-1", .slot = "0", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .slot = "0", .sub_slot = "2"},
                     {.cpv = "app-misc/moving-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/moving-2", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/rdep-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/other-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}}});
    auto targets = request({"app-misc/moving"}, egraph::Selection::update, true);
    targets.reach = {true, true, false, false};
    CHECK(plan(system, egraph::UseRebuilds::none, targets) ==
          std::vector<std::string>{"app-misc/moving-1 -> app-misc/moving-2",
                                   "dev-libs/lib-1 held <- app-misc/rdep-1 dev-libs/lib:0/1=; "
                                   "app-misc/other-1 dev-libs/lib:0/1="});
    // rdep within the reach is rebuilt, and other with it.
    targets.reach = {true, true, true, false};
    CHECK(plan(system, egraph::UseRebuilds::none, targets) ==
          std::vector<std::string>{
              "dev-libs/lib-1 -> dev-libs/lib-2", "app-misc/moving-1 -> app-misc/moving-2",
              "app-misc/rdep-1 -> app-misc/rdep-1 for dev-libs/lib-2 dev-libs/lib:0/1=",
              "app-misc/other-1 -> app-misc/other-1 for dev-libs/lib-2 dev-libs/lib:0/1="});
}

TEST_CASE("outside --deep's reach, a rebuild takes the best version, for run-time bindings only") {
    const auto system =
        make_system({{.cpv = "dev-libs/lib-1", .slot = "0", .sub_slot = "1"},
                     {.cpv = "app-misc/moving-1", .deps = {{"RDEPEND", "dev-libs/lib:0/1="}}},
                     {.cpv = "app-misc/ddep-1", .deps = {{"DEPEND", "dev-libs/lib:0/1="}}}},
                    {{.cpv = "dev-libs/lib-1", .slot = "0", .sub_slot = "1"},
                     {.cpv = "dev-libs/lib-2", .slot = "0", .sub_slot = "2"},
                     {.cpv = "app-misc/moving-1", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/moving-2", .deps = {{"RDEPEND", "dev-libs/lib:="}}},
                     {.cpv = "app-misc/ddep-1", .deps = {{"DEPEND", "dev-libs/lib:="}}}});
    CHECK(plan(system, egraph::UseRebuilds::none,
               request({"dev-libs/lib"}, egraph::Selection::reinstall)) ==
          std::vector<std::string>{
              "dev-libs/lib-1 -> dev-libs/lib-2",
              "app-misc/moving-1 -> app-misc/moving-2 for dev-libs/lib-2 dev-libs/lib:0/1="});
}

TEST_CASE("a new package whose dependency nothing satisfies falls back to another version") {
    const auto system =
        make_system({}, {{.cpv = "app-misc/argfb-1"},
                         {.cpv = "app-misc/argfb-2", .deps = {{"RDEPEND", "dev-libs/missing"}}},
                         {.cpv = "app-misc/puller-1", .deps = {{"RDEPEND", "dev-libs/pulled"}}},
                         {.cpv = "dev-libs/pulled-1"},
                         {.cpv = "dev-libs/pulled-2", .deps = {{"PDEPEND", "dev-libs/missing"}}}});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/argfb"})) ==
          std::vector<std::string>{"new app-misc/argfb-1 <- "});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/puller"})) ==
          std::vector<std::string>{"new app-misc/puller-1 <- ",
                                   "new dev-libs/pulled-1 <- app-misc/puller-1 dev-libs/pulled"});
}

TEST_CASE("an argument no version of which can be merged is unsatisfied, at the chain's end") {
    const auto system =
        make_system({}, {{.cpv = "app-misc/chain-1", .deps = {{"RDEPEND", "dev-libs/link"}}},
                         {.cpv = "dev-libs/link-1", .deps = {{"RDEPEND", "dev-libs/end"}}},
                         {.cpv = "dev-libs/end-1", .deps = {{"BDEPEND", "dev-libs/missing"}}},
                         {.cpv = "app-misc/wants-1", .deps = {{"RDEPEND", "dev-libs/testing"}}},
                         {.cpv = "dev-libs/testing-1", .visible = false},
                         {.cpv = "app-misc/choice-1",
                          .deps = {{"RDEPEND", "|| ( dev-libs/missing dev-libs/there )"}}},
                         {.cpv = "dev-libs/there-1"}});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/chain"})) ==
          std::vector<std::string>{"unsatisfied dev-libs/end-1 dev-libs/missing"});
    // Only a masked version matches.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/wants"})) ==
          std::vector<std::string>{"unsatisfied app-misc/wants-1 dev-libs/testing"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/choice"})) ==
          std::vector<std::string>{"new app-misc/choice-1 <- ",
                                   "new dev-libs/there-1 <- app-misc/choice-1 dev-libs/there"});
}

TEST_CASE("an update whose dependency nothing satisfies is held, unless an argument must have it") {
    const auto system = make_system(
        {{.cpv = "app-misc/upd-1"}, {.cpv = "app-misc/lastupd-1"}, {.cpv = "app-misc/deepupd-1"}},
        {{.cpv = "app-misc/upd-1"},
         {.cpv = "app-misc/upd-2", .deps = {{"RDEPEND", "dev-libs/missing"}}},
         {.cpv = "app-misc/lastupd-2", .deps = {{"RDEPEND", "dev-libs/missing"}}},
         {.cpv = "app-misc/deepupd-1"},
         {.cpv = "app-misc/deepupd-2", .deps = {{"RDEPEND", "dev-libs/link"}}},
         {.cpv = "dev-libs/link-1", .deps = {{"RDEPEND", "dev-libs/end"}}},
         {.cpv = "dev-libs/end-1", .deps = {{"RDEPEND", "dev-libs/missing"}}}},
        {"app-misc/upd", "app-misc/lastupd", "app-misc/deepupd"});
    const egraph::Targets world{.scope = {}, .roots = true, .deep = true};
    CHECK(plan(system, egraph::UseRebuilds::none, world) ==
          std::vector<std::string>{"app-misc/upd-1 held <- app-misc/upd-2 dev-libs/missing",
                                   "app-misc/lastupd-1 held <- app-misc/lastupd-2 dev-libs/missing",
                                   "app-misc/deepupd-1 held <- dev-libs/end-1 dev-libs/missing"});
    // Plain emerge reinstalls the version it has an ebuild of, and must merge the one it has not.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/upd"})) ==
          std::vector<std::string>{"app-misc/upd-1 -> app-misc/upd-1",
                                   "app-misc/upd-1 held <- app-misc/upd-2 dev-libs/missing"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/lastupd"})) ==
          std::vector<std::string>{"unsatisfied app-misc/lastupd-2 dev-libs/missing"});
}

TEST_CASE("a held update is unsatisfiable when nothing satisfies what it needs, not a dependent") {
    const auto system =
        make_system({{.cpv = "app-misc/upd-1"},
                     {.cpv = "app-misc/deepupd-1"},
                     {.cpv = "app-misc/rgb-1"},
                     {.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<app-misc/rgb-2"}}}},
                    {{.cpv = "app-misc/upd-1"},
                     {.cpv = "app-misc/upd-2", .deps = {{"RDEPEND", "dev-libs/missing"}}},
                     {.cpv = "app-misc/deepupd-1"},
                     {.cpv = "app-misc/deepupd-2", .deps = {{"RDEPEND", "dev-libs/end"}}},
                     {.cpv = "dev-libs/end-1", .deps = {{"RDEPEND", "dev-libs/missing"}}},
                     {.cpv = "app-misc/rgb-1"},
                     {.cpv = "app-misc/rgb-2"},
                     {.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<app-misc/rgb-2"}}}},
                    {"app-misc/upd", "app-misc/deepupd", "app-misc/rgb", "app-misc/skin"});
    const egraph::Targets world{.scope = {}, .roots = true, .deep = true};
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none, world);
    std::vector<std::pair<std::string, bool>> held;
    for (const auto& back : found.held) {
        held.emplace_back(store.string(store.packages.at(back.package).cpv), back.unsatisfiable);
    }
    CHECK(held == std::vector<std::pair<std::string, bool>>{{"app-misc/upd-1", true},
                                                            {"app-misc/deepupd-1", true},
                                                            {"app-misc/rgb-1", false}});
    // As plain emerge falls back from what it was asked for.
    const auto asked = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none,
                                            reinstall({"app-misc/upd"}));
    REQUIRE(asked.held.size() == 1);
    CHECK(asked.held.front().unsatisfiable);
    CHECK(egraph::update_lines(store, evaluated, found, egraph::UseRebuilds::none,
                               egraph::HeldLines::unsatisfiable, false, world) ==
          std::vector<std::string>{
              "app-misc/upd-1\theld\tapp-misc/upd-2\ttest_repo\t\tapp-misc/upd-2 dev-libs/missing",
              "app-misc/deepupd-1\theld\tapp-misc/deepupd-2\ttest_repo\t\tdev-libs/end-1 "
              "dev-libs/missing"});
    CHECK(egraph::update_lines(store, evaluated, found, egraph::UseRebuilds::none,
                               egraph::HeldLines::none, false, world)
              .empty());
    CHECK(egraph::update_lines(store, evaluated, found, egraph::UseRebuilds::none,
                               egraph::HeldLines::all, false, world)
              .size() == 3);
}

TEST_CASE("a held update needing a version a dependent holds back is not unsatisfiable") {
    const auto system =
        make_system({{.cpv = "app-misc/host-1"},
                     {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<app-misc/host-2"}}},
                     {.cpv = "app-misc/plugin-1", .deps = {{"RDEPEND", "app-misc/host"}}}},
                    {{.cpv = "app-misc/host-1"},
                     {.cpv = "app-misc/host-2"},
                     {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<app-misc/host-2"}}},
                     {.cpv = "app-misc/plugin-1", .deps = {{"RDEPEND", "app-misc/host"}}},
                     {.cpv = "app-misc/plugin-2", .deps = {{"RDEPEND", ">=app-misc/host-2"}}}},
                    {"app-misc/host", "app-misc/holder", "app-misc/plugin"});
    const egraph::Targets world{.scope = {}, .roots = true, .deep = true};
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none, world);
    REQUIRE(found.held.size() == 2);
    CHECK_FALSE(found.held.at(0).unsatisfiable);
    CHECK_FALSE(found.held.at(1).unsatisfiable);
}

TEST_CASE("an installed package's own missing dependency is left missing") {
    const auto system =
        make_system({{.cpv = "app-misc/broken-1", .deps = {{"RDEPEND", "dev-libs/missing"}}}},
                    {{.cpv = "app-misc/broken-1", .deps = {{"RDEPEND", "dev-libs/missing"}}}},
                    {"app-misc/broken"});
    CHECK(plan(system, egraph::UseRebuilds::none,
               egraph::Targets{.scope = {}, .roots = true, .deep = true})
              .empty());
    // Plain emerge rebuilds it, and cannot.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/broken"})) ==
          std::vector<std::string>{"unsatisfied app-misc/broken-1 dev-libs/missing"});
}

TEST_CASE("update_lines ends with the dependencies nothing satisfies") {
    const auto system =
        make_system({}, {{.cpv = "app-misc/chain-1", .deps = {{"RDEPEND", "dev-libs/missing"}}}});
    const auto targets = reinstall({"app-misc/chain"});
    const auto& [store, evaluated] = system;
    CHECK(egraph::update_lines(store, evaluated, egraph::UseRebuilds::none, egraph::HeldLines::none,
                               false, targets) ==
          std::vector<std::string>{"app-misc/chain-1\tunsatisfied\tdev-libs/missing"});
    CHECK(egraph::update_lines(store, evaluated, egraph::UseRebuilds::none, egraph::HeldLines::none,
                               true, targets) ==
          std::vector<std::string>{"\t\tapp-misc/chain-1\tunsatisfied\tdev-libs/missing"});
}

TEST_CASE("plain emerge refuses an argument with no visible version, installed or not") {
    const auto system = make_system({{.cpv = "app-misc/gone-1"}}, {{.cpv = "app-misc/other-1"}},
                                    {"app-misc/gone", "app-misc/nowhere"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/gone"})) ==
          std::vector<std::string>{"unsatisfied argument app-misc/gone"});
    auto world = reinstall({});
    world.request = {{.set = "selected", .atom = "app-misc/gone"},
                     {.set = "selected", .atom = "app-misc/nowhere"}};
    // Asked for a root set, what it has installed stays, and @selected's atoms may match
    // nothing; @system's may not.
    CHECK(plan(system, egraph::UseRebuilds::none, world).empty());
    world.request.front().set = "system";
    world.request.back().set = "system";
    CHECK(plan(system, egraph::UseRebuilds::none, world) ==
          std::vector<std::string>{"unsatisfied argument app-misc/nowhere"});
    // -u keeps what it has.
    world.selection = egraph::Selection::update;
    CHECK(plan(system, egraph::UseRebuilds::none, world).empty());
}

TEST_CASE("-uD refuses what it reaches and keeps with a run-time dependency nothing satisfies") {
    const auto system =
        make_system({{.cpv = "app-misc/broken-1", .deps = {{"RDEPEND", "dev-libs/missing"}}},
                     {.cpv = "app-misc/builds-1", .deps = {{"BDEPEND", "dev-libs/missing"}}}},
                    {{.cpv = "app-misc/broken-1", .deps = {{"RDEPEND", "dev-libs/missing"}}},
                     {.cpv = "app-misc/builds-1", .deps = {{"BDEPEND", "dev-libs/missing"}}}},
                    {"app-misc/broken", "app-misc/builds"});
    const egraph::Targets world{.scope = {}, .roots = true, .deep = true};
    CHECK(plan(system, egraph::UseRebuilds::none, world).empty());
    // An atom named alone, or every installed package as with @installed.
    auto named = reinstall({"app-misc/broken"}, true);
    named.selection = egraph::Selection::update;
    CHECK(plan(system, egraph::UseRebuilds::none, named) ==
          std::vector<std::string>{"unsatisfied app-misc/broken-1 dev-libs/missing"});
    CHECK(plan(system, egraph::UseRebuilds::none,
               egraph::Targets{.scope = {}, .roots = false, .deep = true}) ==
          std::vector<std::string>{"unsatisfied app-misc/broken-1 dev-libs/missing"});
    // What only fails a USE dependency is refused even for the world sets.
    CHECK(
        plan(make_system({{.cpv = "dev-libs/lib-1"},
                          {.cpv = "app-misc/usedep-1", .deps = {{"RDEPEND", "dev-libs/lib[gtk]"}}}},
                         {}, {"app-misc/usedep"}),
             egraph::UseRebuilds::none,
             world) == std::vector<std::string>{"unsatisfied app-misc/usedep-1 dev-libs/lib[gtk]"});
}

TEST_CASE("a merge whose REQUIRED_USE its USE leaves unsatisfied is refused") {
    const auto system = make_system(
        {{.cpv = "app-misc/requpd-1"}},
        {{.cpv = "app-misc/req-1", .iuse = "a b", .required_use = "^^ ( a b )"},
         {.cpv = "app-misc/reqok-1", .iuse = "a b", .use = "a", .required_use = "^^ ( a b )"},
         {.cpv = "app-misc/requpd-1", .iuse = "a"},
         {.cpv = "app-misc/requpd-2", .iuse = "a", .required_use = "a"},
         {.cpv = "app-misc/reqdep-1", .deps = {{"RDEPEND", "app-misc/req"}}},
         {.cpv = "app-misc/reqchoice-1",
          .deps = {{"RDEPEND", "|| ( app-misc/req app-misc/reqok )"}}}},
        {"app-misc/requpd"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/req"})) ==
          std::vector<std::string>{"new app-misc/req-1 <- ", "unmet app-misc/req-1"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/reqok"})) ==
          std::vector<std::string>{"new app-misc/reqok-1 <- "});
    CHECK(plan(system, egraph::UseRebuilds::none,
               egraph::Targets{.scope = {}, .roots = true, .deep = true}) ==
          std::vector<std::string>{"app-misc/requpd-1 -> app-misc/requpd-2",
                                   "unmet app-misc/requpd-2"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/reqdep"})) ==
          std::vector<std::string>{"new app-misc/req-1 <- app-misc/reqdep-1 app-misc/req",
                                   "new app-misc/reqdep-1 <- ", "unmet app-misc/req-1"});
    // It chooses among a ||'s alternatives regardless.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/reqchoice"})) ==
          std::vector<std::string>{"new app-misc/req-1 <- app-misc/reqchoice-1 app-misc/req",
                                   "new app-misc/reqchoice-1 <- ", "unmet app-misc/req-1"});
    const auto& [store, evaluated] = system;
    CHECK(egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none,
                               reinstall({"app-misc/req"}))
              .refused());
}

TEST_CASE("REQUIRED_USE is weighed wherever emerge selects a version, before its dependencies") {
    const auto system = make_system(
        {{.cpv = "dev-libs/held-1"},
         {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<dev-libs/held-2"}}}},
        {{.cpv = "dev-libs/held-1", .iuse = "a"},
         {.cpv = "dev-libs/held-2", .iuse = "a", .required_use = "a"},
         {.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "<dev-libs/held-2"}}},
         {.cpv = "app-misc/fb-1"},
         {.cpv = "app-misc/fb-2",
          .deps = {{"RDEPEND", "dev-libs/missing"}},
          .iuse = "a",
          .required_use = "a"},
         {.cpv = "dev-libs/badreq-1", .iuse = "a", .required_use = "a"},
         {.cpv = "app-misc/rev-1"},
         {.cpv = "app-misc/rev-2", .deps = {{"RDEPEND", "dev-libs/badreq dev-libs/missing"}}},
         {.cpv = "app-misc/kinds-1"},
         {.cpv = "app-misc/kinds-2",
          .deps = {{"RDEPEND", "dev-libs/missing"}, {"DEPEND", "dev-libs/badreq"}}},
         {.cpv = "app-misc/kinds2-1"},
         {.cpv = "app-misc/kinds2-2",
          .deps = {{"RDEPEND", "dev-libs/badreq"}, {"DEPEND", "dev-libs/missing"}}},
         {.cpv = "app-misc/pd-1"},
         {.cpv = "app-misc/pd-2",
          .deps = {{"RDEPEND", "dev-libs/missing"}, {"PDEPEND", "dev-libs/badreq"}}},
         {.cpv = "app-misc/late-1"},
         {.cpv = "app-misc/late-2",
          .deps = {{"RDEPEND", "dev-libs/missing || ( dev-libs/badreq )"}}}},
        {"app-misc/holder", "dev-libs/held"});
    // An update its dependents hold back.
    CHECK(plan(system, egraph::UseRebuilds::none,
               egraph::Targets{.scope = {}, .roots = true, .deep = true}) ==
          std::vector<std::string>{"dev-libs/held-1 held <- app-misc/holder-1 <dev-libs/held-2",
                                   "unmet dev-libs/held-2"});
    // Reached only through an atom that rejects it, emerge never selects it.
    auto alone = reinstall({"=app-misc/holder-1"}, true);
    alone.selection = egraph::Selection::update;
    CHECK(plan(system, egraph::UseRebuilds::none, alone) ==
          std::vector<std::string>{"dev-libs/held-1 held <- app-misc/holder-1 <dev-libs/held-2"});
    // A version given up for its own missing dependency.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/fb"})) ==
          std::vector<std::string>{"new app-misc/fb-1 <- ", "unmet app-misc/fb-2"});
    // What it would pull in only counts when emerge gets to it: a dependency nothing satisfies
    // comes first within its string, and RDEPEND, IDEPEND, PDEPEND, DEPEND, BDEPEND in turn,
    // with || groups last.
    for (const auto* name : {"app-misc/rev", "app-misc/kinds", "app-misc/pd", "app-misc/late"}) {
        CHECK(plan(system, egraph::UseRebuilds::none, reinstall({name})) ==
              std::vector<std::string>{std::format("new {}-1 <- ", name)});
    }
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/kinds2"})) ==
          std::vector<std::string>{"new app-misc/kinds2-1 <- ", "unmet dev-libs/badreq-1"});
}

TEST_CASE("required_use_of reduces a candidate's REQUIRED_USE under its USE") {
    const auto system = make_system({}, {{.cpv = "app-misc/cond-1",
                                          .iuse = "x a b c",
                                          .use = "x",
                                          .required_use = "x? ( || ( a b ) ) c? ( a ) !x? ( b )"},
                                         {.cpv = "app-misc/old-1",
                                          .iuse = "a",
                                          .required_use = "|| ( )",
                                          .empty_groups_true = true}});
    const auto& evaluated = system.evaluated;
    const auto cond = egraph::required_use_of(evaluated, evaluated.candidates.at(0));
    CHECK_FALSE(cond.satisfied);
    CHECK(cond.unsatisfied == "x? ( || ( a b ) )");
    CHECK(egraph::required_use_of(evaluated, evaluated.candidates.at(1)).satisfied);
}

TEST_CASE("update_lines ends with the REQUIRED_USE left unmet, and all of it when only part is") {
    const auto system = make_system({}, {{.cpv = "app-misc/cond-1",
                                          .iuse = "x a b",
                                          .use = "x",
                                          .required_use = "x? ( || ( a b ) ) !x? ( b )"}});
    const auto targets = reinstall({"app-misc/cond"});
    const auto& [store, evaluated] = system;
    CHECK(egraph::update_lines(store, evaluated, egraph::UseRebuilds::none, egraph::HeldLines::none,
                               false, targets) ==
          std::vector<std::string>{
              "app-misc/cond-1\tnew\tapp-misc/cond-1\ttest_repo\tUSE=\"x -a -b\"\tapp-misc/cond",
              "app-misc/cond-1\trequired-use\ttest_repo\tUSE=\"x -a -b\"\tx? ( || ( a b ) )"
              "\tx? ( || ( a b ) ) !x? ( b )"});
}

TEST_CASE("a USE dependency nothing meets as built takes a USE change, as autounmask asks") {
    const auto system = make_system(
        {}, {{.cpv = "dev-libs/lib-1", .iuse = "gtk qt"},
             {.cpv = "dev-libs/lib-2",
              .deps = {{"RDEPEND", "gtk? ( dev-libs/gtkdep )"}},
              .iuse = "gtk qt"},
             {.cpv = "dev-libs/gtkdep-1"},
             {.cpv = "dev-libs/other-1"},
             {.cpv = "app-misc/wantgtk-1", .deps = {{"RDEPEND", "dev-libs/lib[gtk]"}}},
             {.cpv = "app-misc/wantnoqt-1", .deps = {{"RDEPEND", "dev-libs/lib[-qt,gtk]"}}},
             {.cpv = "app-misc/anyof-1",
              .deps = {{"RDEPEND", "|| ( dev-libs/lib[gtk] dev-libs/other )"}}},
             {.cpv = "app-misc/anyof2-1",
              .deps = {{"RDEPEND", "|| ( dev-libs/lib[gtk] dev-libs/nothere )"}}}});
    // Its dependencies follow the changed USE.
    CHECK(
        plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/wantgtk"})) ==
        std::vector<std::string>{"new app-misc/wantgtk-1 <- ",
                                 "new dev-libs/gtkdep-1 <- dev-libs/lib-2 dev-libs/gtkdep",
                                 "new dev-libs/lib-2 <- app-misc/wantgtk-1 dev-libs/lib[gtk]",
                                 "use dev-libs/lib-2 gtk <- app-misc/wantgtk-1 dev-libs/lib[gtk]"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/wantnoqt"})).back() ==
          "use dev-libs/lib-2 gtk <- app-misc/wantnoqt-1 dev-libs/lib[-qt,gtk]");
    // A || takes an alternative that needs none, else changes its first.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/anyof"})) ==
          std::vector<std::string>{"new app-misc/anyof-1 <- ",
                                   "new dev-libs/other-1 <- app-misc/anyof-1 dev-libs/other"});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/anyof2"})).back() ==
          "use dev-libs/lib-2 gtk <- app-misc/anyof2-1 dev-libs/lib[gtk]");
    // An argument's atom too.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"dev-libs/lib[gtk]"})).back() ==
          "use dev-libs/lib-2 gtk <- dev-libs/lib[gtk]");
    const auto& [store, evaluated] = system;
    const auto found = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none,
                                            reinstall({"app-misc/wantgtk"}));
    CHECK(found.refused());
    REQUIRE(found.changed);
    const auto& lib = found.changed->candidates.at(found.use_changes.front().change.candidate);
    CHECK(found.changed->string(found.changed->ids_in(lib.use).front()) == "gtk");
}

TEST_CASE("autounmask rebuilds an installed version, and passes over a version it cannot change") {
    const auto system = make_system(
        {{.cpv = "dev-libs/lib-1"}},
        {{.cpv = "dev-libs/lib-1", .iuse = "gtk qt"},
         {.cpv = "app-misc/wantgtk-1", .deps = {{"RDEPEND", "dev-libs/lib[gtk]"}}},
         {.cpv = "dev-libs/pinned-1", .iuse = "x"},
         {.cpv = "dev-libs/pinned-2", .iuse = "x", .forced = "x"},
         {.cpv = "app-misc/wantx-1", .deps = {{"RDEPEND", "dev-libs/pinned[x]"}}},
         {.cpv = "app-misc/wantmissing-1", .deps = {{"RDEPEND", "dev-libs/pinned[nope]"}}}});
    CHECK(
        plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/wantgtk"})) ==
        std::vector<std::string>{"dev-libs/lib-1 -> dev-libs/lib-1", "new app-misc/wantgtk-1 <- ",
                                 "use dev-libs/lib-1 gtk <- app-misc/wantgtk-1 dev-libs/lib[gtk]"});
    CHECK(
        plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/wantx"})) ==
        std::vector<std::string>{"new app-misc/wantx-1 <- ",
                                 "new dev-libs/pinned-1 <- app-misc/wantx-1 dev-libs/pinned[x]",
                                 "use dev-libs/pinned-1 x <- app-misc/wantx-1 dev-libs/pinned[x]"});
    // A flag outside IUSE, with no default, no change can meet.
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"app-misc/wantmissing"})) ==
          std::vector<std::string>{"unsatisfied app-misc/wantmissing-1 dev-libs/pinned[nope]"});
}

TEST_CASE("update_lines ends with the package.use lines a plan needs, and what needs them") {
    const auto system = make_system(
        {}, {{.cpv = "dev-libs/lib-1", .iuse = "gtk qt"},
             {.cpv = "dev-libs/lib-2", .iuse = "gtk qt"},
             {.cpv = "dev-libs/lib-3", .iuse = "gtk qt", .forced = "gtk"},
             {.cpv = "dev-libs/lib-4", .visible = false, .iuse = "gtk qt"},
             {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/wantgtk"}}},
             {.cpv = "app-misc/wantgtk-1", .deps = {{"RDEPEND", "dev-libs/lib[gtk,-qt]"}}}});
    const auto targets = reinstall({"app-misc/user"});
    const auto& [store, evaluated] = system;
    const auto lines = egraph::update_lines(store, evaluated, egraph::UseRebuilds::none,
                                            egraph::HeldLines::none, false, targets);
    // The profile fixes gtk on lib-3 and lib-4 is masked; lib-3 is newer, so "=".
    CHECK(lines.back() == "dev-libs/lib-2\tuse-change\ttest_repo\t=dev-libs/lib-2 gtk"
                          "\tapp-misc/wantgtk-1::test_repo\tapp-misc/user-1::test_repo"
                          "\tapp-misc/user (argument)");
    // The merge shows the USE it is built with.
    CHECK(std::ranges::contains(
        lines, std::string{"dev-libs/lib-2\tnew\tdev-libs/lib-2\ttest_repo\tUSE=\"gtk -qt\"\t"
                           "app-misc/wantgtk-1 dev-libs/lib[gtk,-qt]"}));
}

TEST_CASE("package_use_line takes >= when nothing visible or installed is newer") {
    const auto system = make_system({{.cpv = "dev-libs/lib-3", .slot = "3"}},
                                    {{.cpv = "dev-libs/lib-1", .iuse = "gtk"},
                                     {.cpv = "dev-libs/lib-2", .slot = "2", .iuse = "gtk"},
                                     {.cpv = "dev-libs/lib-5", .visible = false, .iuse = "gtk"}});
    const auto& [store, evaluated] = system;
    const auto line = [&](std::string_view cpv) {
        for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
            if (evaluated.string(evaluated.candidates.at(i).cpv) == cpv) {
                return egraph::package_use_line(
                    store, evaluated, {.candidate = i, .flags = {{"gtk", true}, {"qt", false}}});
            }
        }
        return std::string{};
    };
    // lib-3 is installed in its own slot; lib-5 is masked.
    CHECK(line("dev-libs/lib-2") == ">=dev-libs/lib-2:2 gtk -qt");
    CHECK(line("dev-libs/lib-1") == ">=dev-libs/lib-1:0 gtk -qt");
}

TEST_CASE("an installed build a USE dependency accepts holds an update back, with no USE change") {
    const auto system =
        make_system({{.cpv = "dev-libs/gcr-1", .iuse = "gtk", .use = "gtk"},
                     {.cpv = "app-misc/keyring-1", .deps = {{"RDEPEND", "dev-libs/gcr[gtk]"}}}},
                    {{.cpv = "dev-libs/gcr-1", .iuse = "gtk"},
                     {.cpv = "dev-libs/gcr-2", .iuse = "gtk"},
                     {.cpv = "app-misc/keyring-1", .deps = {{"RDEPEND", "dev-libs/gcr[gtk]"}}}},
                    {"app-misc/keyring", "dev-libs/gcr"});
    CHECK(plan(system, egraph::UseRebuilds::none,
               egraph::Targets{.scope = {}, .roots = true, .deep = true}) ==
          std::vector<std::string>{"dev-libs/gcr-1 held <- app-misc/keyring-1 dev-libs/gcr[gtk]"});
}

TEST_CASE("an argument's USE dependencies take a USE change over an installed version") {
    const auto system = make_system(
        {{.cpv = "dev-libs/lib-1", .iuse = "gtk qt"}},
        {{.cpv = "dev-libs/lib-1", .iuse = "gtk qt"}, {.cpv = "dev-libs/lib-2", .iuse = "gtk qt"}});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"dev-libs/lib[gtk]"})) ==
          std::vector<std::string>{"dev-libs/lib-1 -> dev-libs/lib-2",
                                   "use dev-libs/lib-2 gtk <- dev-libs/lib[gtk]"});
}

TEST_CASE("a USE dependency the installed version was built without rebuilds it as configured") {
    // As dev-db/redis[jemalloc] needs dev-libs/jemalloc[stats] once package.use enables stats.
    const auto system =
        make_system({{.cpv = "dev-libs/jemalloc-5", .iuse = "stats"}},
                    {{.cpv = "dev-libs/jemalloc-5", .iuse = "stats", .use = "stats"},
                     {.cpv = "dev-db/redis-8", .deps = {{"RDEPEND", "dev-libs/jemalloc[stats]"}}}});
    CHECK(plan(system, egraph::UseRebuilds::none, reinstall({"dev-db/redis"})) ==
          std::vector<std::string>{"dev-libs/jemalloc-5 -> dev-libs/jemalloc-5",
                                   "new dev-db/redis-8 <- "});
    // Even an older version, when the installed one's ebuild is gone.
    const auto older =
        make_system({{.cpv = "dev-libs/jemalloc-6", .iuse = "stats"}},
                    {{.cpv = "dev-libs/jemalloc-5", .iuse = "stats", .use = "stats"},
                     {.cpv = "dev-db/redis-8", .deps = {{"RDEPEND", "dev-libs/jemalloc[stats]"}}}});
    CHECK(plan(older, egraph::UseRebuilds::none, reinstall({"dev-db/redis"})) ==
          std::vector<std::string>{"dev-libs/jemalloc-6 -> dev-libs/jemalloc-5",
                                   "new dev-db/redis-8 <- "});
}
