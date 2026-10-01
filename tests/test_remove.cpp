#include "remove.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::test::make_system;

namespace {

std::vector<std::uint32_t> ids_of(const egraph::Store& store,
                                  const std::vector<std::string>& cpvs) {
    std::vector<std::uint32_t> ids;
    for (const auto& cpv : cpvs) {
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (store.string(store.packages.at(id).cpv) == cpv) {
                ids.push_back(id);
            }
        }
    }
    REQUIRE(ids.size() == cpvs.size());
    return ids;
}

std::vector<std::string> removing(const egraph::test::System& system,
                                  const std::vector<std::string>& cpvs) {
    const auto& store = system.store;
    return egraph::removal_lines(store, egraph::plan_removal(store, {}, ids_of(store, cpvs)));
}

// app-misc/a and app-misc/b in @selected, a needing dev-libs/lib; sys-apps/base in @system,
// needing dev-libs/core.
egraph::test::System sample() {
    return make_system({{.cpv = "app-misc/a-1", .deps = {{"RDEPEND", "dev-libs/lib"}}},
                        {.cpv = "app-misc/b-1"},
                        {.cpv = "dev-libs/lib-1"},
                        {.cpv = "dev-libs/core-1"},
                        {.cpv = "sys-apps/base-1", .deps = {{"RDEPEND", "dev-libs/core"}}}},
                       {}, {"app-misc/a", "app-misc/b"}, {"sys-apps/base"});
}

} // namespace

TEST_CASE("a matched package nothing else needs is removed, @selected or not") {
    const auto system = sample();
    CHECK(removing(system, {"app-misc/b-1"}) == std::vector<std::string>{"app-misc/b-1\tremove"});
    CHECK(removing(system, {"app-misc/a-1", "dev-libs/lib-1"}) ==
          std::vector<std::string>{"app-misc/a-1\tremove", "dev-libs/lib-1\tremove"});
}

TEST_CASE("a matched package something unmatched needs is kept, with what keeps it") {
    const auto system = sample();
    CHECK(removing(system, {"dev-libs/lib-1"}) ==
          std::vector<std::string>{"dev-libs/lib-1\tkept\tapp-misc/a-1"});
    CHECK(removing(system, {"sys-apps/base-1", "dev-libs/core-1"}) ==
          std::vector<std::string>{"dev-libs/core-1\tkept\tsys-apps/base-1",
                                   "sys-apps/base-1\tkept\t@system"});
}

TEST_CASE("a set world_sets names keeps its atoms when @selected goes") {
    auto system = sample();
    auto& store = system.store;
    // app-misc/b's atom as the world file and a nested set would both give it.
    const auto via = static_cast<std::uint32_t>(store.strings.size());
    store.strings.push_back({.first = static_cast<std::uint32_t>(store.pool.size()), .count = 5});
    store.pool += "myset";
    for (auto& root : store.roots) {
        if (store.string(root.atom) == "app-misc/b") {
            root.via = via;
        }
    }
    CHECK(removing(system, {"app-misc/b-1"}) ==
          std::vector<std::string>{"app-misc/b-1\tkept\t@myset"});
}

TEST_CASE("removing keeps what the options take as gone out of the roots") {
    const auto system = sample();
    const auto& store = system.store;
    egraph::KeepOptions options;
    options.removed.assign(store.packages.size(), false);
    options.removed.at(ids_of(store, {"app-misc/a-1"}).front()) = true;
    CHECK(egraph::removal_lines(
              store, egraph::plan_removal(store, options, ids_of(store, {"dev-libs/lib-1"}))) ==
          std::vector<std::string>{"dev-libs/lib-1\tremove"});
}

TEST_CASE("depclean's removals are read from its selected packages") {
    CHECK(egraph::parse_depclean(">>> No packages selected for removal by depclean\n").empty());
    CHECK(egraph::parse_depclean("\n>>> These are the packages that would be unmerged:\n\n"
                                 " app-misc/c\n    selected: 1 2 \n\n"
                                 "All selected packages: =app-misc/c-2 =app-misc/c-1\n\n"
                                 "Number to remove:     2\n") ==
          std::vector<std::string>{"app-misc/c-1", "app-misc/c-2"});
}

TEST_CASE("removals differ by package, ours first") {
    const auto system = sample();
    const auto& store = system.store;
    const auto removal = egraph::plan_removal(store, {}, ids_of(store, {"app-misc/b-1"}));
    CHECK(egraph::removal_differences(store, removal, std::vector<std::string>{"app-misc/b-1"})
              .empty());
    CHECK(egraph::removal_differences(store, removal, std::vector<std::string>{"app-misc/a-1"}) ==
          std::vector<std::string>{"app-misc/a-1\temerge\tremove", "app-misc/b-1\tegraph\tremove"});
}

TEST_CASE("a depclean keeps emerge's build-time and dynamic dependency defaults unless told") {
    CHECK(egraph::depclean_options(true, true).empty());
    CHECK(egraph::depclean_options(false, false) ==
          std::vector<std::string>{"--with-bdeps=n", "--dynamic-deps=n"});
}
