#include "depclean.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string_view>
#include <utility>

#include <string>
#include <vector>

using egraph::test::Bytes;

namespace {

egraph::Store decoded(const std::vector<std::byte>& bytes) {
    auto store = egraph::decode(bytes);
    REQUIRE(store.has_value());
    return std::move(*store);
}

} // namespace

// The sample: app-misc/a-1 in @selected, RDEPEND "|| ( dev-libs/b dev-libs/missing )
// !app-misc/old", and dev-libs/b-1.
TEST_CASE("depclean keeps the roots and the alternative they use") {
    const auto store = decoded(egraph::test::fresh_sample());
    const auto kept = egraph::keep(store, {});
    CHECK(kept.packages == std::vector<bool>{true, true});
    REQUIRE(kept.roots.size() == 1);
    CHECK((kept.roots.front().root == 0 && kept.roots.front().child == 0));
    // The installed alternative, from inside the || group; the blocker is not followed.
    CHECK(kept.pulls == std::vector<egraph::Edge>{
                            {.parent = 0, .child = 1, .kind = 4, .atom = 7, .choice = true}});
    CHECK(kept.unresolved.empty());
    CHECK(egraph::orphans(kept).empty());
}

TEST_CASE("without roots everything is an orphan") {
    const auto store = decoded(egraph::test::with_section(5, Bytes{}.varint(0)));
    const auto kept = egraph::keep(store, {});
    CHECK(egraph::orphans(kept) == std::vector<std::uint32_t>{0, 1});
}

TEST_CASE("a runtime dependency nothing satisfies is unresolved") {
    // a-1 alone, RDEPEND: dev-libs/missing dev-libs/b.
    const auto store = decoded(egraph::test::with_rdepend(2, [](Bytes& nodes) {
        nodes.varints({0, 0, 14}).list({});
        nodes.varints({0, 0, 7}).list({});
    }));
    const auto kept = egraph::keep(store, {});
    CHECK(egraph::unresolved_lines(store, kept) ==
          std::vector<std::string>{"app-misc/a-1\tRDEPEND\tdev-libs/b",
                                   "app-misc/a-1\tRDEPEND\tdev-libs/missing"});
}

TEST_CASE("why follows kept dependencies back to a root") {
    const auto store = decoded(egraph::test::fresh_sample());
    const auto kept = egraph::keep(store, {});
    const auto path = egraph::why(kept, 1);
    REQUIRE(path.has_value());
    CHECK(egraph::path_lines(store, path.value_or(egraph::Path{})) ==
          std::vector<std::string>{"@selected\tapp-misc/a\tapp-misc/a-1",
                                   "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1\tany-of"});

    const auto rootless = decoded(egraph::test::with_section(5, Bytes{}.varint(0)));
    CHECK_FALSE(egraph::why(egraph::keep(rootless, {}), 1).has_value());
}

namespace {

// app-misc/a-1 in @selected with RDEPEND written by rdepend, and dev-libs/b-1 and third, a
// package of cp (string 17) with cpv string 18.
std::vector<std::byte> three(std::string_view cp, std::string_view third,
                             const std::function<void(Bytes&)>& rdepend) {
    Bytes packages;
    packages.varint(3);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0);
    rdepend(packages);
    packages.varint(0).varint(0);
    const std::uint64_t third_cp = cp == "dev-libs/b" ? 7 : 17;
    for (const auto& [cpv, cp_id] : {std::pair<std::uint64_t, std::uint64_t>{8, 7},
                                     std::pair<std::uint64_t, std::uint64_t>{18, third_cp}}) {
        packages.varints({cpv, cp_id, 3, 3, 4, 5, 1});
        packages.list({}).list({}).varint(0);
        packages.varint(0).varint(0).varint(0).varint(0).varint(0);
        packages.varint(0).varint(0);
    }
    return egraph::test::with_strings({cp, third}, 4, packages);
}

std::vector<std::uint32_t> orphaned(const egraph::Store& store,
                                    std::vector<egraph::Masking> masking) {
    return egraph::orphans(
        egraph::keep(store, {.build_deps = true, .masking = std::move(masking)}));
}

} // namespace

TEST_CASE("a || group passes over a masked package no visible ebuild backs") {
    // RDEPEND: || ( dev-libs/b dev-libs/c ), both installed.
    const auto store = decoded(three("dev-libs/c", "dev-libs/c-1", [](Bytes& nodes) {
        nodes.varint(3);
        nodes.varints({1, 0, 0}).list({});
        nodes.varints({0, 1, 7}).list({1});
        nodes.varints({0, 1, 17}).list({2});
    }));
    CHECK(orphaned(store, {}) == std::vector<std::uint32_t>{2});
    const egraph::Masking unmasked;
    const egraph::Masking masked{.masked = true, .visible = false};
    CHECK(orphaned(store, {unmasked, masked, unmasked}) == std::vector<std::uint32_t>{1});
    // A visible ebuild of its version keeps it available.
    CHECK(orphaned(store, {unmasked, {.masked = true, .visible = true}, unmasked}) ==
          std::vector<std::uint32_t>{2});
    // Unmasked, it is available even without one.
    CHECK(orphaned(store, {unmasked, {.masked = false, .visible = false}, unmasked}) ==
          std::vector<std::uint32_t>{2});
}

TEST_CASE("an atom selects an unmasked installed match, then a visible one") {
    // RDEPEND: dev-libs/b, matching b-1 and b-2.
    const auto store = decoded(three("dev-libs/b", "dev-libs/b-2", [](Bytes& nodes) {
        nodes.varint(1);
        nodes.varints({0, 0, 7}).list({1, 2});
    }));
    const egraph::Masking unmasked;
    const egraph::Masking masked{.masked = true, .visible = true};
    CHECK(orphaned(store, {}) == std::vector<std::uint32_t>{1});
    CHECK(orphaned(store, {unmasked, unmasked, masked}) == std::vector<std::uint32_t>{2});
    CHECK(orphaned(store, {unmasked, unmasked, {.masked = false, .visible = false}}) ==
          std::vector<std::uint32_t>{2});
    // Both masked: the highest.
    CHECK(orphaned(store, {unmasked, masked, masked}) == std::vector<std::uint32_t>{1});
}
