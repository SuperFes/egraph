#include "evaluated.hpp"
#include "freshness.hpp"
#include "graph.hpp"
#include "helpers.hpp"
#include "query.hpp"
#include "store.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

enum class Beside : std::uint8_t { nothing, unsatisfied, satisfied };

// The installed sample with app-misc/a-1 depending on dev-libs/b-1 through atom (string 17), at
// the top level or as an alternative of a || beside dev-libs/missing (unsatisfied) or app-misc/a
// (satisfied by a-1).
egraph::Store depending_through(std::string_view atom, Beside beside) {
    Bytes packages;
    packages.varint(2);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0);
    if (beside == Beside::nothing) {
        packages.varint(1).varints({0, 0, 17}).list({1});
    } else {
        packages.varint(3);
        packages.varints({1, 0, 0}).list({});
        packages.varints({0, 1, 17}).list({1});
        if (beside == Beside::unsatisfied) {
            packages.varints({0, 1, 14}).list({});
        } else {
            packages.varints({0, 1, 2}).list({0});
        }
    }
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    auto store = egraph::decode(egraph::test::with_strings({atom}, 4, packages));
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
    const auto possible = evaluated->possible_in(a.possible);
    REQUIRE(possible.size() == 2);
    CHECK(egraph::dep_kinds.at(possible.front().kind) == "RDEPEND");
    CHECK(evaluated->string(possible.front().atom) == "dev-libs/b");
    CHECK_FALSE(possible.front().choice);
    CHECK(evaluated->ids_in(possible.front().matches).front() == 1);
    CHECK(possible.back().choice);
    CHECK(evaluated->ids_in(possible.back().matches).empty());
    CHECK(evaluated->string(evaluated->ids_in(possible.back().flags).back()) == "-minimal");
    CHECK(a.visible);
    CHECK_FALSE(a.masked);
    CHECK(a.vdb_masked);
    CHECK(a.target == 0);
    REQUIRE(evaluated->ids_in(a.rebuild).size() == 2);
    CHECK(evaluated->string(evaluated->ids_in(a.rebuild).front()) == "flag*");
    const auto& b = evaluated->packages.back();
    CHECK(b.source == egraph::DepSource::vdb);
    CHECK(evaluated->possible_in(b.possible).empty());
    CHECK_FALSE(b.visible);
    CHECK(b.masked);
    CHECK(b.vdb_masked);
    CHECK(b.target == 2);
    CHECK(evaluated->ids_in(b.rebuild).empty());

    REQUIRE(evaluated->candidates.size() == 3);
    const auto& visible = evaluated->candidates.front();
    CHECK(evaluated->string(visible.cpv) == "app-misc/a-1");
    CHECK(visible.visible());
    CHECK(evaluated->ids_in(visible.use).size() == 1);
    CHECK(std::ranges::equal(evaluated->ids_in(visible.forced), std::array{8U}));
    const auto& masked = evaluated->candidates.at(1);
    CHECK_FALSE(masked.visible());
    CHECK(evaluated->string(evaluated->ids_in(masked.reasons).front()) == "~amd64 keyword");
    for (const auto deps : masked.deps) {
        CHECK(deps.count == 0);
    }
    const auto& b2 = evaluated->candidates.back();
    CHECK(evaluated->pairs_in(b2.errors).empty());
    const auto depend = evaluated->nodes_in(b2.deps.at(1));
    REQUIRE(depend.size() == 1);
    CHECK(evaluated->string(depend.front().atom) == "app-misc/a");
    CHECK(std::ranges::equal(evaluated->ids_in(depend.front().matches), std::array{0U}));
    CHECK(evaluated->nodes_in(b2.deps.at(4)).empty());
    const auto strings = [&](egraph::Range range) {
        std::vector<std::string_view> found;
        for (const auto id : evaluated->ids_in(range)) {
            found.push_back(evaluated->string(id));
        }
        return found;
    };
    CHECK(strings(evaluated->repository_cps) ==
          std::vector<std::string_view>{"app-misc/a", "dev-libs/b"});
    CHECK(strings(evaluated->requested) == std::vector<std::string_view>{"dev-libs/gone"});
    CHECK(strings(evaluated->use_expand) ==
          std::vector<std::string_view>{"python_targets", "video_cards"});
    CHECK(strings(evaluated->use_expand_hidden) == std::vector<std::string_view>{"video_cards"});
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

    Bytes bad_kind;
    bad_kind.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0});
    bad_kind.varint(1).varints({5, 13, 0}).list({}).list({});
    CHECK_THAT(rejection(evaluated_with_section(4, bad_kind)),
               Catch::Matchers::StartsWith("dependencies: kind 5 out of range 5"));

    Bytes bad_choice;
    bad_choice.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0});
    bad_choice.varint(1).varints({4, 13, 2}).list({}).list({});
    CHECK_THAT(rejection(evaluated_with_section(4, bad_choice)),
               Catch::Matchers::StartsWith("dependencies: choice 2 out of range 2"));

    Bytes bad_visible;
    bad_visible.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0, 0});
    bad_visible.varints({2, 0, 0, 0}).list({});
    CHECK_THAT(rejection(evaluated_with_section(4, bad_visible)),
               Catch::Matchers::StartsWith("dependencies: visible 2 out of range 2"));

    Bytes bad_target;
    bad_target.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0, 0});
    bad_target.varints({1, 0, 0, 4}).list({});
    CHECK_THAT(rejection(evaluated_with_section(4, bad_target)),
               Catch::Matchers::StartsWith("dependencies: candidate 4 out of range 4"));

    Bytes bad_masked;
    bad_masked.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0, 0});
    bad_masked.varints({1, 0, 2, 0}).list({});
    CHECK_THAT(rejection(evaluated_with_section(4, bad_masked)),
               Catch::Matchers::StartsWith("dependencies: masked 2 out of range 2"));

    // Two dependency records, so package 2 does not exist.
    Bytes bad_candidate_match;
    bad_candidate_match.varint(1).varints({5, 1, 7, 6, 6}).list({}).list({}).list({}).list({});
    bad_candidate_match.varint(0);
    bad_candidate_match.varints({0, 0, 0, 0, 1}).varints({0, 0, 4}).list({2});
    CHECK_THAT(rejection(evaluated_with_section(5, bad_candidate_match)),
               Catch::Matchers::StartsWith("candidates: package 2 out of range 2"));

    Bytes trailing;
    trailing.varint(0).varint(0);
    CHECK_THAT(rejection(evaluated_with_section(5, trailing)),
               Catch::Matchers::StartsWith("candidates: trailing bytes"));

    CHECK_THAT(rejection(evaluated_with_section(6, Bytes{}.list({99}))),
               Catch::Matchers::StartsWith("repository: string 99 out of range"));
    CHECK_THAT(rejection(evaluated_with_section(6, Bytes{}.list({13, 5}))),
               Catch::Matchers::StartsWith("repository: cps out of order"));
    CHECK_THAT(rejection(evaluated_with_section(7, Bytes{}.list({14, 14}))),
               Catch::Matchers::StartsWith("requested: cps out of order"));
    CHECK_THAT(rejection(evaluated_with_section(8, Bytes{}.list({99}).list({}))),
               Catch::Matchers::StartsWith("use_expand: string 99 out of range"));
    CHECK_THAT(rejection(evaluated_with_section(8, Bytes{}.list({}))),
               Catch::Matchers::StartsWith("use_expand: "));
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
    swapped.varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    swapped.varints({1, 1, 3}).varint(0).varints({0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    egraph::test::write_bytes(path, evaluated_with_section(4, swapped));
    const auto mismatched = egraph::load_evaluated(path, store);
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().message ==
          path.string() + ": package 1 is app-misc/a-1, not the installed store's dev-libs/b-1");

    Bytes one;
    one.varint(1).varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
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

TEST_CASE("dynamic dependencies replace the installed trees") {
    auto evaluated = egraph::decode_evaluated(sample());
    REQUIRE(evaluated.has_value());
    const auto store = egraph::with_dynamic_deps(installed(), *evaluated);

    REQUIRE(store.packages.size() == 2);
    const auto& a = store.packages.front();
    CHECK(store.string(a.cpv) == "app-misc/a-1");
    const auto rdepend = store.nodes_in(a.deps.at(4));
    REQUIRE(rdepend.size() == 1);
    CHECK(store.string(rdepend.front().atom) == "dev-libs/b:=");
    REQUIRE(store.ids_in(rdepend.front().matches).size() == 1);
    CHECK(store.ids_in(rdepend.front().matches).front() == 1);
    // The installed RDEPEND error is the vdb's; the evaluated one replaces it.
    REQUIRE(store.pairs_in(a.errors).size() == 1);
    CHECK(store.string(store.pairs_in(a.errors).front().second) == "bad dep");
    // Sonames and roots are the installed store's.
    CHECK(store.required_in(a.required).size() == 1);
    CHECK(store.roots.size() == 2);
    CHECK(store.string(store.roots.front().atom) == "app-misc/a");
}

TEST_CASE("groups keep the empty atom through the merge") {
    Bytes dependencies;
    dependencies.varint(2);
    dependencies.varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 2});
    dependencies.varints({1, 0, 0}).list({});
    dependencies.varints({0, 1, 4}).list({1}).varints({0, 1, 0, 0, 0, 0});
    dependencies.varints({2, 1, 3}).varint(0).varints({0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    auto evaluated = egraph::decode_evaluated(evaluated_with_section(4, dependencies));
    REQUIRE(evaluated.has_value());
    const auto store = egraph::with_dynamic_deps(installed(), *evaluated);
    const auto rdepend = store.nodes_in(store.packages.front().deps.at(4));
    REQUIRE(rdepend.size() == 2);
    CHECK(rdepend.front().type == egraph::NodeType::any_of);
    CHECK(rdepend.front().atom == 0);
    CHECK(rdepend.back().parent == 0);
    CHECK(store.string(rdepend.back().atom) == "dev-libs/b:=");
    CHECK(store.pairs_in(store.packages.front().errors).empty());
}

TEST_CASE("possible dependencies are listed with their toggles") {
    const auto evaluated = egraph::decode_evaluated(sample());
    REQUIRE(evaluated.has_value());
    // dev-libs/gone matches nothing installed, so it has no line.
    const std::vector<std::string> a{"app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1\tuse=flag"};
    CHECK(egraph::possible_lines(*evaluated, std::vector<std::uint32_t>{0}, false) == a);
    CHECK(egraph::possible_lines(*evaluated, std::vector<std::uint32_t>{1}, true) == a);
    CHECK(egraph::possible_lines(*evaluated, std::vector<std::uint32_t>{0}, true).empty());
    CHECK(egraph::possible_lines(*evaluated, std::vector<std::uint32_t>{1}, false).empty());
}

TEST_CASE("updates are what emerge -u would replace, and rebuild for USE when asked") {
    const auto evaluated = egraph::decode_evaluated(sample());
    REQUIRE(evaluated.has_value());
    const auto store = installed();
    const auto lines = [&](egraph::UseRebuilds rebuilds) {
        return egraph::update_lines(store, *evaluated, rebuilds);
    };
    const std::vector<std::string> upgrade{"dev-libs/b-1\tupgrade\tdev-libs/b-2\ttest_repo"};
    CHECK(lines(egraph::UseRebuilds::none) == upgrade);
    CHECK(lines(egraph::UseRebuilds::changed) ==
          std::vector<std::string>{"app-misc/a-1\trebuild\tapp-misc/a-1\ttest_repo\tflag*",
                                   upgrade.front()});
    CHECK(lines(egraph::UseRebuilds::all) ==
          std::vector<std::string>{"app-misc/a-1\trebuild\tapp-misc/a-1\ttest_repo\tflag* -new%",
                                   upgrade.front()});
}

// b-1's target is b-2, in slot 0/0; nothing else of dev-libs/b is visible. (tests/test_plan.cpp
// weighs holds in full.)
TEST_CASE("updates list what dependents hold back, and who") {
    const auto evaluated = egraph::decode_evaluated(sample());
    REQUIRE(evaluated.has_value());
    const auto lines = [&](std::string_view atom, Beside beside = Beside::nothing) {
        return egraph::update_lines(depending_through(atom, beside), *evaluated,
                                    egraph::UseRebuilds::none, true);
    };
    const std::vector<std::string> upgrade{"dev-libs/b-1\tupgrade\tdev-libs/b-2\ttest_repo"};
    const std::vector<std::string> held{
        "dev-libs/b-1\theld\tdev-libs/b-2\ttest_repo\t\tapp-misc/a-1 <dev-libs/b-2"};
    CHECK(lines("dev-libs/b") == upgrade);
    CHECK(lines("<dev-libs/b-2") == held);
    // A || holds unless another alternative is installed or can be pulled in; nothing can.
    CHECK(lines("<dev-libs/b-2", Beside::unsatisfied) == held);
    CHECK(lines("<dev-libs/b-2", Beside::satisfied) == upgrade);
}

TEST_CASE("an update's kind follows the versions") {
    const auto lines = [](std::uint64_t visible, std::uint64_t target) {
        Bytes dependencies;
        dependencies.varint(2);
        dependencies.varints({1, 0, 3}).varint(0).varints({0, 0, 0, 0, 0, 0});
        dependencies.varints({visible, 0, 0, target}).list({});
        dependencies.varints({2, 1, 3}).varint(0).varints({0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
        const auto evaluated = egraph::decode_evaluated(evaluated_with_section(4, dependencies));
        REQUIRE(evaluated.has_value());
        return egraph::update_lines(installed(), *evaluated, egraph::UseRebuilds::none);
    };
    CHECK(lines(1, 2) ==
          std::vector<std::string>{"app-misc/a-1\tupgrade\tapp-misc/a-2\ttest_repo"});
    // Masked, and replaced by the same version from the target's repository.
    CHECK(lines(0, 1) ==
          std::vector<std::string>{"app-misc/a-1\trebuild\tapp-misc/a-1\ttest_repo"});
}
