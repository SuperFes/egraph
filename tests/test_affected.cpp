#include "affected.hpp"

#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

// app-misc/a-1 RDEPEND "|| ( dev-libs/b dev-libs/missing ) !app-misc/old", requiring
// libb.so.1, which dev-libs/b-1 provides.
egraph::Store sample() {
    auto store = egraph::decode(egraph::test::fresh_sample());
    REQUIRE(store.has_value());
    return std::move(*store);
}

using Cpvs = std::vector<std::string>;

} // namespace

TEST_CASE("an affected request is an object of string lists") {
    const auto request = egraph::parse_request(
        R"({"kinds": ["RDEPEND", "SONAME"], "seeds": ["app-misc/a-1"], "changed": ["dev-libs/b"],
            "replaced": [], "blockers": ["!app-misc/old"]})");
    REQUIRE(request.has_value());
    CHECK(request->kinds == Cpvs{"RDEPEND", "SONAME"});
    CHECK(request->seeds == Cpvs{"app-misc/a-1"});
    CHECK(request->changed == Cpvs{"dev-libs/b"});
    CHECK(request->blockers == Cpvs{"!app-misc/old"});
    CHECK(egraph::parse_request("{}").has_value());
    CHECK_FALSE(egraph::parse_request("[]").has_value());
    CHECK_FALSE(egraph::parse_request(R"({"seeds": "app-misc/a-1"})").has_value());
    CHECK_FALSE(egraph::parse_request(R"({"seeds": [1]})").has_value());
    CHECK_FALSE(egraph::parse_request(R"({"kinds": ["RUNTIME"]})").has_value());
}

TEST_CASE("loose matches compare version and slot, not USE or blocking") {
    const auto store = sample();
    CHECK(egraph::loose_matches(store, "dev-libs/b") == std::vector<std::uint32_t>{1});
    CHECK(egraph::loose_matches(store, "!!dev-libs/b[foo,-bar,baz?]") ==
          std::vector<std::uint32_t>{1});
    CHECK(egraph::loose_matches(store, "~dev-libs/b-1:0=") == std::vector<std::uint32_t>{1});
    CHECK(egraph::loose_matches(store, ">=dev-libs/b-2").empty());
    CHECK(egraph::loose_matches(store, "dev-libs/b:1").empty());
    CHECK(egraph::loose_matches(store, "not an atom").empty());
}

TEST_CASE("reachable follows the asked kinds, every any-of member included") {
    const auto store = sample();
    const auto reach = [&](Cpvs kinds, Cpvs seeds) {
        return egraph::affected(store, {.kinds = std::move(kinds),
                                        .seeds = std::move(seeds),
                                        .changed = {},
                                        .replaced = {},
                                        .blockers = {}})
            .reachable;
    };
    CHECK(reach({"RDEPEND"}, {"app-misc/a-1"}) == Cpvs{"app-misc/a-1", "dev-libs/b-1"});
    CHECK(reach({"SONAME"}, {"app-misc/a-1"}) == Cpvs{"app-misc/a-1", "dev-libs/b-1"});
    CHECK(reach({"DEPEND"}, {"app-misc/a-1"}) == Cpvs{"app-misc/a-1"});
    CHECK(reach({"RDEPEND"}, {"dev-libs/b-1", "x/not-installed-1"}) == Cpvs{"dev-libs/b-1"});
}

TEST_CASE("affected are what depends on a change, is blocked, or loses a soname") {
    const auto store = sample();
    const auto answer = [&](Cpvs changed, Cpvs replaced, Cpvs blockers) {
        return egraph::affected(store, {.kinds = {},
                                        .seeds = {},
                                        .changed = std::move(changed),
                                        .replaced = std::move(replaced),
                                        .blockers = std::move(blockers)});
    };
    CHECK(answer({"dev-libs/b"}, {}, {}).affected == Cpvs{"app-misc/a-1"});
    // Any edge naming the cp counts: one nothing installed satisfies, and a blocker.
    CHECK(answer({"dev-libs/missing"}, {}, {}).affected == Cpvs{"app-misc/a-1"});
    CHECK(answer({"app-misc/old"}, {}, {}).affected == Cpvs{"app-misc/a-1"});
    CHECK(answer({"app-misc/a"}, {}, {}).affected.empty());
    CHECK(answer({}, {"dev-libs/b-1"}, {}).affected == Cpvs{"app-misc/a-1"});
    CHECK(answer({}, {"app-misc/a-1"}, {}).affected.empty());
    // A blocked package is affected itself, and so are its own dependents.
    const auto blocked = answer({}, {}, {"!dev-libs/b"});
    CHECK(blocked.blocked == Cpvs{"dev-libs/b-1"});
    CHECK(blocked.affected == Cpvs{"app-misc/a-1", "dev-libs/b-1"});
}

TEST_CASE("the answer is JSON with sorted keys") {
    CHECK(egraph::to_json({.reachable = {"a/b-1"}, .blocked = {}, .affected = {"c/d-2"}}) ==
          R"({"affected":["c/d-2"],"blocked":[],"reachable":["a/b-1"]})"
          "\n");
}
