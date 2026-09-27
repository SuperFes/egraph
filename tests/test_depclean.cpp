#include "depclean.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

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
