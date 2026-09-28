#include "evaluated.hpp"
#include "freshness.hpp"
#include "helpers.hpp"
#include "store.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <vector>

using egraph::test::Bytes;
using egraph::test::evaluated_with_section;

namespace {

std::vector<std::byte> sample() {
    return egraph::test::assemble_evaluated(egraph::test::evaluated_sections());
}

egraph::Store installed() {
    auto store = egraph::decode(egraph::test::assemble(egraph::test::sample_sections()));
    REQUIRE(store.has_value());
    return std::move(*store);
}

std::string rejection(const std::vector<std::byte>& bytes) {
    const auto evaluated = egraph::decode_evaluated(bytes);
    REQUIRE_FALSE(evaluated.has_value());
    return evaluated.error().message;
}

} // namespace

TEST_CASE("the sample evaluated store decodes") {
    const auto evaluated = egraph::decode_evaluated(sample());
    REQUIRE(evaluated.has_value());
    CHECK(evaluated->meta.build_time_ns == 43);
    CHECK(evaluated->meta.installed_build_time_ns == 42);
    REQUIRE(evaluated->inputs.size() == 1);
    CHECK(evaluated->inputs.front().path == "/var/db/repos/gentoo");

    REQUIRE(evaluated->packages.size() == 2);
    const auto& a = evaluated->packages.front();
    CHECK(evaluated->string(a.cpv) == "app-misc/a-1");
    CHECK(a.source == egraph::DepSource::ebuild);
    CHECK(evaluated->string(a.eapi) == "8");
    REQUIRE(evaluated->pairs_in(a.errors).size() == 1);
    CHECK(evaluated->string(evaluated->pairs_in(a.errors).front().first) == "RDEPEND");
    const auto rdepend = evaluated->nodes_in(a.deps.at(4));
    REQUIRE(rdepend.size() == 1);
    CHECK(evaluated->string(rdepend.front().atom) == "dev-libs/b:=");
    CHECK(evaluated->ids_in(rdepend.front().matches).front() == 1);
    CHECK(evaluated->packages.back().source == egraph::DepSource::vdb);

    REQUIRE(evaluated->candidates.size() == 2);
    const auto& visible = evaluated->candidates.front();
    CHECK(evaluated->string(visible.cpv) == "app-misc/a-1");
    CHECK(visible.visible());
    CHECK(evaluated->ids_in(visible.use).size() == 1);
    const auto& masked = evaluated->candidates.back();
    CHECK_FALSE(masked.visible());
    CHECK(evaluated->string(evaluated->ids_in(masked.reasons).front()) == "~amd64 keyword");
}

TEST_CASE("every evaluated truncation is rejected") {
    const auto bytes = sample();
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const std::vector<std::byte> prefix(bytes.begin(),
                                            bytes.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK_FALSE(egraph::decode_evaluated(prefix).has_value());
    }
}

// Run under ASan+UBSan: whatever the bytes, decode returns rather than crashing.
TEST_CASE("corrupted bytes never crash the evaluated decoder") {
    const auto bytes = sample();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (const std::byte value :
             {std::byte{0x00}, std::byte{0xFF}, std::byte{0x80}, bytes.at(i) ^ std::byte{0x01}}) {
            auto corrupt = bytes;
            corrupt.at(i) = value;
            const auto evaluated = egraph::decode_evaluated(corrupt);
            CHECK((evaluated.has_value() || !evaluated.error().message.empty()));
        }
    }
}

TEST_CASE("the two stores are told apart") {
    CHECK(rejection(egraph::test::assemble(egraph::test::sample_sections())) ==
          "not an evaluated egraph store");
    const auto store = egraph::decode(sample());
    REQUIRE_FALSE(store.has_value());
    CHECK(store.error().message == "not an egraph store");
}

TEST_CASE("evaluated records are checked") {
    Bytes bad_source;
    bad_source.varint(1).varints({1, 3, 3}).varint(0).varints({0, 0, 0, 0, 0});
    CHECK(rejection(evaluated_with_section(4, bad_source)) ==
          "dependencies: source 3 out of range 3 at byte 3");

    // One record, so package 1 does not exist.
    Bytes bad_match;
    bad_match.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 1});
    bad_match.varints({0, 0, 4}).list({1});
    CHECK_THAT(rejection(evaluated_with_section(4, bad_match)),
               Catch::Matchers::StartsWith("dependencies: package 1 out of range 1"));

    Bytes bad_reason;
    bad_reason.varint(1).varints({5, 1, 7, 6, 6}).list({}).list({}).list({99});
    CHECK_THAT(rejection(evaluated_with_section(5, bad_reason)),
               Catch::Matchers::StartsWith("candidates: string 99 out of range"));

    Bytes trailing;
    trailing.varint(0).varint(0);
    CHECK_THAT(rejection(evaluated_with_section(5, trailing)),
               Catch::Matchers::StartsWith("candidates: trailing bytes"));
}

TEST_CASE("an evaluated store loads only beside its installed store") {
    const egraph::test::TempDir dir;
    const auto path = dir.path() / "installed.evaluated.egraph";
    const auto store = installed();

    egraph::test::write_bytes(path, sample());
    const auto loaded = egraph::load_evaluated(path, store);
    REQUIRE(loaded.has_value());
    CHECK(loaded->packages.size() == store.packages.size());

    // b-1's record names another package.
    Bytes swapped;
    swapped.varint(2);
    swapped.varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0});
    swapped.varints({1, 1, 3}).varint(0).varints({0, 0, 0, 0, 0});
    egraph::test::write_bytes(path, evaluated_with_section(4, swapped));
    const auto mismatched = egraph::load_evaluated(path, store);
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().message ==
          path.string() + ": package 1 is app-misc/a-1, not the installed store's dev-libs/b-1");

    Bytes one;
    one.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0});
    egraph::test::write_bytes(path, evaluated_with_section(4, one));
    const auto short_one = egraph::load_evaluated(path, store);
    REQUIRE_FALSE(short_one.has_value());
    CHECK(short_one.error().message == path.string() + ": 1 packages, the installed store has 2");

    egraph::test::write_bytes(path, sample());
    const auto missing = egraph::load_evaluated(dir.path() / "none", store);
    REQUIRE_FALSE(missing.has_value());
    CHECK_THAT(missing.error().message, Catch::Matchers::ContainsSubstring("none"));
}

TEST_CASE("an evaluated store is stale once its installed store is rebuilt") {
    const auto store = installed();
    auto evaluated = egraph::decode_evaluated(evaluated_with_section(2, Bytes{}.varint(0)));
    REQUIRE(evaluated.has_value());
    CHECK_FALSE(egraph::staleness(*evaluated, store).has_value());
    evaluated->meta.installed_build_time_ns = 41;
    CHECK(egraph::staleness(*evaluated, store) == "built against another installed store");
}

TEST_CASE("the evaluated store sits beside the installed one") {
    CHECK(egraph::evaluated_store_path("/var/cache/egraph/installed.egraph") ==
          "/var/cache/egraph/installed.evaluated.egraph");
    CHECK(egraph::evaluated_store_path("scratch") == "scratch.evaluated.egraph");
    CHECK(egraph::evaluated_store_path("a.b/store.x") == "a.b/store.evaluated.egraph");
}
