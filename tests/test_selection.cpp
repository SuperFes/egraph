#include "selection.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::test::make_system;

namespace {

// user (needing lib) and leaf in @selected, with base in @system.
egraph::test::System sample() {
    return make_system({{.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "dev-libs/lib"}}},
                        {.cpv = "app-misc/leaf-1"},
                        {.cpv = "dev-libs/lib-1"},
                        {.cpv = "sys-apps/base-1", .deps = {{"RDEPEND", "dev-libs/lib"}}}},
                       {}, {"app-misc/user", "app-misc/leaf"}, {"sys-apps/base"});
}

// Gives the root of atom the world_sets set name.
void give_via(egraph::Store& store, std::string_view atom, std::string_view name) {
    const auto via = static_cast<std::uint32_t>(store.strings.size());
    store.strings.push_back({.first = static_cast<std::uint32_t>(store.pool.size()),
                             .count = static_cast<std::uint32_t>(name.size())});
    store.pool += name;
    for (auto& root : store.roots) {
        if (store.string(root.atom) == atom) {
            root.via = via;
        }
    }
}

} // namespace

TEST_CASE("the world file's atoms are @selected's own") {
    auto system = sample();
    using Atoms = std::set<std::string, std::less<>>;
    CHECK(egraph::world_atoms(system.store) == Atoms{"app-misc/leaf", "app-misc/user"});
    give_via(system.store, "app-misc/leaf", "myset");
    CHECK(egraph::world_atoms(system.store) == Atoms{"app-misc/user"});
}

TEST_CASE("selection changes are what joined and what left") {
    using Atoms = std::set<std::string, std::less<>>;
    CHECK(egraph::selection_changes(Atoms{"a/b", "c/d"}, Atoms{"a/b", "c/d"}).empty());
    CHECK(egraph::selection_changes(Atoms{"a/b", "c/d"}, Atoms{"c/d", "e/f"}) ==
          std::vector<std::string>{"a/b\tdeselected", "e/f\tselected"});
}

TEST_CASE("deselect's atoms are read from emerge's own words") {
    CHECK(egraph::parse_deselect(">>> No matching atoms found in \"world\" favorites file...\n")
              .empty());
    CHECK(egraph::parse_deselect(">>> Would remove app-misc/user from \"world\" favorites "
                                 "file...\n>>> Would remove @myset from \"world_sets\" favorites "
                                 "file...\n>>> Would remove a/b:2 from \"world\" favorites "
                                 "file...\n") ==
          std::vector<std::string>{"@myset", "a/b:2", "app-misc/user"});
}

TEST_CASE("a deselect lists what then becomes an orphan, not what already is") {
    auto system = sample();
    const auto& store = system.store;
    const std::vector<std::string> user{"app-misc/user"};
    // lib stays for base.
    CHECK(egraph::deselect_lines(store, {}, user) ==
          std::vector<std::string>{"app-misc/user\tdeselect", "app-misc/user-1\torphan"});
    const std::vector<std::string> both{"app-misc/leaf", "app-misc/user"};
    CHECK(egraph::deselect_lines(store, {}, both) ==
          std::vector<std::string>{"app-misc/leaf\tdeselect", "app-misc/user\tdeselect",
                                   "app-misc/leaf-1\torphan", "app-misc/user-1\torphan"});
}

TEST_CASE("deselecting a world_sets set drops the atoms it gives, not the world file's") {
    auto system = sample();
    give_via(system.store, "app-misc/leaf", "myset");
    const std::vector<std::string> set{"@myset"};
    CHECK(egraph::deselect_lines(system.store, {}, set) ==
          std::vector<std::string>{"@myset\tdeselect", "app-misc/leaf-1\torphan"});
    const std::vector<std::string> leaf{"app-misc/leaf"};
    CHECK(egraph::deselect_lines(system.store, {}, leaf) ==
          std::vector<std::string>{"app-misc/leaf\tdeselect"});
}
