#include "blockers.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

using egraph::test::Available;
using egraph::test::Installed;
using egraph::test::make_system;

namespace {

egraph::Store conflicted() {
    return egraph::test::make_system(
               {
                   Installed{.cpv = "app-misc/holder-1",
                             .deps = {{"RDEPEND", "!app-misc/old !!dev-libs/gone"},
                                      {"DEPEND", "!app-misc/tool || ( !app-misc/x app-misc/y )"}}},
                   Installed{.cpv = "app-misc/old-1"},
                   Installed{.cpv = "app-misc/other-1", .deps = {{"PDEPEND", "!<app-misc/old-1"}}},
                   Installed{.cpv = "app-misc/tool-1"},
                   Installed{.cpv = "app-misc/x-1"},
                   Installed{.cpv = "app-misc/y-1"},
               },
               {})
        .store;
}

} // namespace

TEST_CASE("blockers without packages lists those matching something installed") {
    CHECK(egraph::blocker_lines(conflicted(), {}) ==
          std::vector<std::string>{"app-misc/holder-1\tDEPEND\t!app-misc/tool\tapp-misc/tool-1",
                                   "app-misc/holder-1\tDEPEND\t!app-misc/x\tapp-misc/x-1",
                                   "app-misc/holder-1\tRDEPEND\t!app-misc/old\tapp-misc/old-1"});
}

TEST_CASE("blockers of named packages lists what they hold and what holds them") {
    const auto store = conflicted();
    // holder-1: each of its blockers, !!dev-libs/gone matching nothing.
    CHECK(egraph::blocker_lines(store, std::vector<std::uint32_t>{0}) ==
          std::vector<std::string>{"app-misc/holder-1\tDEPEND\t!app-misc/tool\tapp-misc/tool-1",
                                   "app-misc/holder-1\tDEPEND\t!app-misc/x\tapp-misc/x-1",
                                   "app-misc/holder-1\tRDEPEND\t!!dev-libs/gone\t",
                                   "app-misc/holder-1\tRDEPEND\t!app-misc/old\tapp-misc/old-1"});
    // old-1: blocked by holder-1; other-1's blocker spares it.
    CHECK(egraph::blocker_lines(store, std::vector<std::uint32_t>{1}) ==
          std::vector<std::string>{"app-misc/holder-1\tRDEPEND\t!app-misc/old\tapp-misc/old-1"});
    CHECK(egraph::blocker_lines(store, std::vector<std::uint32_t>{2}) ==
          std::vector<std::string>{"app-misc/other-1\tPDEPEND\t!<app-misc/old-1\t"});
    CHECK(egraph::blocker_lines(store, std::vector<std::uint32_t>{5}).empty());
}

namespace {

std::string cpv(const egraph::test::System& system, const egraph::Member& member) {
    return std::string(
        member.candidate ? system.evaluated.string(system.evaluated.candidates.at(member.index).cpv)
                         : system.store.string(system.store.packages.at(member.index).cpv));
}

std::string block(const egraph::test::System& system, const egraph::Block& found) {
    return std::format("{} {} {}", cpv(system, found.holder), found.atom,
                       cpv(system, found.blocked));
}

// "merge cpv" per merge, "uninstall cpv: holder atom blocked" per uninstall, then
// "block holder atom blocked" per block.
std::vector<std::string> outcome(const egraph::test::System& system,
                                 const egraph::Targets& targets) {
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none, targets);
    std::vector<std::string> lines;
    for (const auto& merge : plan.merges) {
        lines.push_back(std::format(
            "merge {}",
            system.evaluated.string(system.evaluated.candidates.at(merge.candidate).cpv)));
    }
    for (const auto& each : plan.uninstalls) {
        lines.push_back(std::format("uninstall {}: {}",
                                    system.store.string(system.store.packages.at(each.package).cpv),
                                    block(system, each.why)));
    }
    for (const auto& each : plan.blocks) {
        lines.push_back("block " + block(system, each));
    }
    return lines;
}

// emerge -u @world, or -uD.
egraph::Targets world(bool deep = false) {
    return {.scope = {}, .roots = true, .deep = deep};
}

// emerge -u @installed.
egraph::Targets installed() {
    return {.scope = {}, .roots = false, .deep = false};
}

// Plain emerge with atoms as its arguments.
egraph::Targets request(const std::vector<std::string>& atoms) {
    egraph::Targets targets{.scope = {}, .roots = true, .deep = false};
    for (const auto& atom : atoms) {
        targets.request.push_back({.set = "", .atom = atom});
    }
    targets.selection = egraph::Selection::reinstall;
    return targets;
}

// A package renamed to app-misc/new, which blocks the old name; its one user moves over.
egraph::test::System renamed(std::string_view blocker, std::vector<std::string> world_set) {
    return make_system(
        {Installed{.cpv = "app-misc/old-1"},
         Installed{.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/old"}}}},
        {Available{.cpv = "app-misc/old-1"},
         Available{.cpv = "app-misc/new-1", .deps = {{"RDEPEND", std::string(blocker)}}},
         Available{.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/old"}}},
         Available{.cpv = "app-misc/user-2", .deps = {{"RDEPEND", "app-misc/new"}}}},
        world_set);
}

} // namespace

TEST_CASE("a merge's weak blocker uninstalls what nothing needs any more") {
    CHECK(outcome(renamed("!app-misc/old", {"app-misc/user"}), world()) ==
          std::vector<std::string>{
              "merge app-misc/user-2", "merge app-misc/new-1",
              "uninstall app-misc/old-1: app-misc/new-1 !app-misc/old app-misc/old-1"});
    // @world selects it, or @installed names every installed package as an argument.
    CHECK(outcome(renamed("!app-misc/old", {"app-misc/user", "app-misc/old"}), world()) ==
          std::vector<std::string>{"merge app-misc/user-2", "merge app-misc/new-1",
                                   "block app-misc/new-1 !app-misc/old app-misc/old-1"});
    CHECK(outcome(renamed("!app-misc/old", {"app-misc/user"}), installed()) ==
          std::vector<std::string>{"merge app-misc/user-2", "merge app-misc/new-1",
                                   "block app-misc/new-1 !app-misc/old app-misc/old-1"});
}

TEST_CASE("a strong blocker is resolved only by a replacement on the running root") {
    const auto strong = renamed("!!app-misc/old", {"app-misc/user"});
    CHECK(outcome(strong, world()) ==
          std::vector<std::string>{"merge app-misc/user-2", "merge app-misc/new-1",
                                   "block app-misc/new-1 !!app-misc/old app-misc/old-1"});
    auto elsewhere = world();
    elsewhere.running_root = false;
    CHECK(outcome(strong, elsewhere) ==
          std::vector<std::string>{
              "merge app-misc/user-2", "merge app-misc/new-1",
              "uninstall app-misc/old-1: app-misc/new-1 !!app-misc/old app-misc/old-1"});

    // A version another merge replaces needs nothing, whatever the strength.
    for (const auto* blocker : {"!<dev-libs/lib-2", "!!<dev-libs/lib-2"}) {
        const auto replaced =
            make_system({Installed{.cpv = "dev-libs/lib-1"},
                         Installed{.cpv = "app-misc/app-1", .deps = {{"RDEPEND", "dev-libs/lib"}}}},
                        {Available{.cpv = "dev-libs/lib-1"}, Available{.cpv = "dev-libs/lib-2"},
                         Available{.cpv = "app-misc/app-1", .deps = {{"RDEPEND", "dev-libs/lib"}}},
                         Available{.cpv = "app-misc/app-2",
                                   .deps = {{"RDEPEND", std::string(blocker) + " dev-libs/lib"}}}},
                        {"app-misc/app"});
        CHECK(outcome(replaced, world(true)) ==
              std::vector<std::string>{"merge dev-libs/lib-2", "merge app-misc/app-2"});
        // Plain -u keeps lib-1, which app-2 needs.
        CHECK(outcome(replaced, world()) ==
              std::vector<std::string>{
                  "merge app-misc/app-2",
                  std::format("block app-misc/app-2 {} dev-libs/lib-1", blocker)});
    }

    // In a merge's own slot a weak blocker is ignored, and a strong one on the version it
    // replaces can never be resolved.
    for (const auto* blocker : {"!<app-misc/p-2", "!!<app-misc/p-2"}) {
        const auto self =
            make_system({Installed{.cpv = "app-misc/p-1"}},
                        {Available{.cpv = "app-misc/p-1"},
                         Available{.cpv = "app-misc/p-2", .deps = {{"RDEPEND", blocker}}}},
                        {"app-misc/p"});
        auto expected = std::vector<std::string>{"merge app-misc/p-2"};
        if (std::string_view(blocker).starts_with("!!")) {
            expected.push_back(std::format("block app-misc/p-2 {} app-misc/p-1", blocker));
        }
        CHECK(outcome(self, world(true)) == expected);
    }
}

TEST_CASE("an installed package's run-time blocker against a merge uninstalls it") {
    const auto holding = [](std::string_view kind, std::vector<std::string> world_set) {
        return make_system(
            {Installed{.cpv = "app-misc/holder-1",
                       .deps = {{std::string(kind), "!app-misc/fresh"}}},
             Installed{.cpv = "app-misc/top-1"}},
            {Available{.cpv = "app-misc/fresh-1"}, Available{.cpv = "app-misc/top-1"},
             Available{.cpv = "app-misc/top-2", .deps = {{"RDEPEND", "app-misc/fresh"}}}},
            world_set);
    };
    CHECK(outcome(holding("RDEPEND", {"app-misc/top"}), world()) ==
          std::vector<std::string>{
              "merge app-misc/top-2", "merge app-misc/fresh-1",
              "uninstall app-misc/holder-1: app-misc/holder-1 !app-misc/fresh app-misc/fresh-1"});
    CHECK(outcome(holding("RDEPEND", {"app-misc/top", "app-misc/holder"}), world()) ==
          std::vector<std::string>{"merge app-misc/top-2", "merge app-misc/fresh-1",
                                   "block app-misc/holder-1 !app-misc/fresh app-misc/fresh-1"});
    // An installed package's build-time blockers are behind it.
    CHECK(outcome(holding("DEPEND", {"app-misc/top", "app-misc/holder"}), world()) ==
          std::vector<std::string>{"merge app-misc/top-2", "merge app-misc/fresh-1"});

    // Blockers between installed packages no merge touches are the damage already done.
    const auto pair = [](std::vector<Available> available) {
        return make_system({Installed{.cpv = "app-misc/x-1", .deps = {{"RDEPEND", "!app-misc/y"}}},
                            Installed{.cpv = "app-misc/y-1"}},
                           std::move(available), {"app-misc/x", "app-misc/y"});
    };
    CHECK(outcome(pair({}), world()).empty());
    CHECK(outcome(pair({Available{.cpv = "app-misc/y-1"}, Available{.cpv = "app-misc/y-2"}}),
                  world()) ==
          std::vector<std::string>{"merge app-misc/y-2",
                                   "block app-misc/x-1 !app-misc/y app-misc/y-2"});
}

TEST_CASE("emerge's completed graph keeps what it reaches, through every kind") {
    // top selects x through each kind of dependency, or an orphan depends on it.
    const auto needing = [](std::vector<Installed> installed) {
        installed.push_back(Installed{.cpv = "app-misc/x-1"});
        std::ranges::sort(installed, {}, &Installed::cpv);
        return make_system(
            installed, {Available{.cpv = "app-misc/fresh-1", .deps = {{"RDEPEND", "!app-misc/x"}}}},
            {"app-misc/top"});
    };
    const auto fresh = request({"app-misc/fresh"});
    for (const auto* kind : {"BDEPEND", "DEPEND", "IDEPEND", "PDEPEND", "RDEPEND"}) {
        CHECK(outcome(needing({Installed{.cpv = "app-misc/top-1", .deps = {{kind, "app-misc/x"}}}}),
                      fresh) ==
              std::vector<std::string>{"merge app-misc/fresh-1",
                                       "block app-misc/fresh-1 !app-misc/x app-misc/x-1"});
    }
    CHECK(
        outcome(needing({Installed{.cpv = "app-misc/top-1"},
                         Installed{.cpv = "app-misc/orph-1", .deps = {{"RDEPEND", "app-misc/x"}}}}),
                fresh) == std::vector<std::string>{
                              "merge app-misc/fresh-1",
                              "uninstall app-misc/x-1: app-misc/fresh-1 !app-misc/x app-misc/x-1"});
    // A || takes its first alternative left satisfied.
    const auto choosing = [&](std::string_view group) {
        return needing(
            {Installed{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", std::string(group)}}},
             Installed{.cpv = "app-misc/y-1"}});
    };
    CHECK(outcome(choosing("|| ( app-misc/x app-misc/y )"), fresh) ==
          std::vector<std::string>{"merge app-misc/fresh-1",
                                   "block app-misc/fresh-1 !app-misc/x app-misc/x-1"});
    CHECK(outcome(choosing("|| ( app-misc/y app-misc/x )"), fresh) ==
          std::vector<std::string>{
              "merge app-misc/fresh-1",
              "uninstall app-misc/x-1: app-misc/fresh-1 !app-misc/x app-misc/x-1"});
    // A merge's own dependency, and an argument, keep it too.
    const auto pulled =
        make_system({Installed{.cpv = "app-misc/x-1"}},
                    {Available{.cpv = "app-misc/fresh-1", .deps = {{"RDEPEND", "!app-misc/x"}}},
                     Available{.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/x"}}}});
    CHECK(outcome(pulled, request({"app-misc/fresh", "app-misc/user"})) ==
          std::vector<std::string>{"merge app-misc/fresh-1", "merge app-misc/user-1",
                                   "block app-misc/fresh-1 !app-misc/x app-misc/x-1"});
}

TEST_CASE("an uninstall waits for every merge whose blocker needs it gone") {
    // "uninstall cpv: merge cpv..." per uninstall.
    const auto waits = [](const egraph::test::System& system) {
        const auto plan = egraph::plan_updates(system.store, system.evaluated,
                                               egraph::UseRebuilds::none, world());
        std::vector<std::string> lines;
        for (const auto& each : plan.uninstalls) {
            auto line = std::format(
                "uninstall {}:", system.store.string(system.store.packages.at(each.package).cpv));
            for (const auto merge : each.after) {
                line += std::format(
                    " {}",
                    system.evaluated.string(
                        system.evaluated.candidates.at(plan.merges.at(merge).candidate).cpv));
            }
            lines.push_back(std::move(line));
        }
        return lines;
    };
    CHECK(waits(renamed("!app-misc/old", {"app-misc/user"})) ==
          std::vector<std::string>{"uninstall app-misc/old-1: app-misc/new-1"});
    // holder-1 blocks the merge top-2 pulls in, and top-2 blocks holder-1.
    const auto both = make_system(
        {Installed{.cpv = "app-misc/holder-1", .deps = {{"RDEPEND", "!app-misc/fresh"}}},
         Installed{.cpv = "app-misc/top-1"}},
        {Available{.cpv = "app-misc/fresh-1"}, Available{.cpv = "app-misc/top-1"},
         Available{.cpv = "app-misc/top-2",
                   .deps = {{"RDEPEND", "app-misc/fresh !app-misc/holder"}}}},
        {"app-misc/top"});
    CHECK(waits(both) ==
          std::vector<std::string>{"uninstall app-misc/holder-1: app-misc/top-2 app-misc/fresh-1"});
}

TEST_CASE("a blocker between two merges is a block") {
    const auto system =
        make_system({}, {Available{.cpv = "app-misc/a-1", .deps = {{"RDEPEND", "!app-misc/b"}}},
                         Available{.cpv = "app-misc/b-1"}});
    CHECK(outcome(system, request({"app-misc/a", "app-misc/b"})) ==
          std::vector<std::string>{"merge app-misc/a-1", "merge app-misc/b-1",
                                   "block app-misc/a-1 !app-misc/b app-misc/b-1"});
}

TEST_CASE("-u's greedy slots leave out an installed slot the best version blocks") {
    const auto slotted = [](std::string_view blocker) {
        return make_system({Installed{.cpv = "dev-libs/s-1", .slot = "1"}},
                           {Available{.cpv = "dev-libs/s-1", .slot = "1"},
                            Available{.cpv = "dev-libs/s-1.1", .slot = "1"},
                            Available{.cpv = "dev-libs/s-2",
                                      .deps = {{"RDEPEND", std::string(blocker)}},
                                      .slot = "2"}},
                           {"dev-libs/s"});
    };
    CHECK(outcome(slotted("!dev-libs/s:1"), world()) ==
          std::vector<std::string>{
              "merge dev-libs/s-2",
              "uninstall dev-libs/s-1: dev-libs/s-2 !dev-libs/s:1 dev-libs/s-1"});
    CHECK(outcome(slotted("!dev-libs/other"), world()) ==
          std::vector<std::string>{"merge dev-libs/s-1.1", "merge dev-libs/s-2"});
}

TEST_CASE("update lines end with the uninstalls, then the blocks") {
    const auto moved = renamed("!app-misc/old", {"app-misc/user"});
    CHECK(egraph::update_lines(moved.store, moved.evaluated, egraph::UseRebuilds::none, false,
                               false, world()) ==
          std::vector<std::string>{
              "app-misc/user-1\tupgrade\tapp-misc/user-2\ttest_repo",
              "app-misc/new-1\tnew\tapp-misc/new-1\ttest_repo\t\tapp-misc/user-2 app-misc/new",
              "app-misc/old-1\tuninstall\tapp-misc/new-1\t!app-misc/old\tapp-misc/old-1"});
    // In the table, after the place of the merge it waits for.
    const auto moved_table = egraph::update_lines(moved.store, moved.evaluated,
                                                  egraph::UseRebuilds::none, false, true, world());
    REQUIRE(moved_table.size() == 3);
    CHECK(moved_table.at(0).starts_with("1\t\tapp-misc/new-1\t"));
    CHECK(moved_table.back() ==
          "\t1\tapp-misc/old-1\tuninstall\tapp-misc/new-1\t!app-misc/old\tapp-misc/old-1");
    const auto kept = renamed("!app-misc/old", {"app-misc/user", "app-misc/old"});
    const auto table = egraph::update_lines(kept.store, kept.evaluated, egraph::UseRebuilds::none,
                                            false, true, world());
    REQUIRE(table.size() == 3);
    CHECK(table.back() == "\t\tapp-misc/new-1\tblocks\t!app-misc/old\tapp-misc/old-1");
}
