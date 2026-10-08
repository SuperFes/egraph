#include "atom.hpp"
#include "store_writer.hpp"
#include "version.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using egraph::Operator;
using egraph::UseDependency;

namespace {

int compare(std::string_view a, std::string_view b) {
    const auto x = egraph::parse_version(a);
    const auto y = egraph::parse_version(b);
    REQUIRE(x.has_value());
    REQUIRE(y.has_value());
    if (!x || !y) {
        return 0;
    }
    return egraph::vercmp(*x, *y);
}

egraph::Store decoded(const std::vector<std::byte>& bytes) {
    auto store = egraph::decode(bytes);
    REQUIRE(store.has_value());
    return std::move(*store);
}

egraph::Evaluated decoded_evaluated(const std::vector<std::byte>& bytes) {
    auto evaluated = egraph::decode_evaluated(bytes);
    REQUIRE(evaluated.has_value());
    return std::move(*evaluated);
}

egraph::Atom parsed(std::string_view text) {
    auto atom = egraph::parse_atom(text);
    REQUIRE(atom.has_value());
    if (!atom) {
        return {};
    }
    return std::move(*atom);
}

} // namespace

// Expectations are portage.versions.vercmp's signs.
TEST_CASE("versions compare as portage compares them") {
    const std::vector<std::tuple<std::string_view, std::string_view, int>> cases{
        {"1.0", "1.0", 0},
        {"1.0", "1.0.0", -1},
        {"1.0-r1", "1.0", 1},
        {"1.0-r0", "1.0", 0},
        {"1.02", "1.1", -1},
        {"1.010", "1.01", 0},
        {"1.0a", "1.0", 1},
        {"1.0a", "1.0b", -1},
        {"1.0_alpha", "1.0_beta", -1},
        {"1.0_rc1", "1.0", -1},
        {"1.0", "1.0_p0", -1},
        {"1.0_p", "1.0", 1},
        {"1.0_p0", "1.0_p", 0},
        {"1_pre2", "1_pre10", -1},
        {"10", "9", 1},
        {"01", "1", 0},
        {"1.0_alpha_p1", "1.0_alpha", 1},
        {"12.2.5", "12.2b", 1},
        {"99999999999999999999", "100000000000000000000", -1},
        {"1.0-r2", "1.0-r10", -1},
        {"1.0.0", "1.0a", 1},
        {"2.4_rc3_p1", "2.4_rc3", 1},
        {"1_p1_alpha", "1_p1", -1},
    };
    for (const auto& [a, b, expected] : cases) {
        INFO(a << " vs " << b);
        CHECK(compare(a, b) == expected);
        CHECK(compare(b, a) == -expected);
    }
}

TEST_CASE("malformed versions are rejected") {
    for (const std::string_view bad :
         {"", "a", "1.", "1..2", "1_foo", "1-r", "1-r1a", "1ab", "1.0-1"}) {
        INFO(bad);
        CHECK_FALSE(egraph::parse_version(bad).has_value());
    }
    const auto version = egraph::parse_version("1.2.3b_rc4_p-r06").value_or(egraph::Version{});
    CHECK(version.base == "1.2.3b_rc4_p");
    CHECK(version.revision == "06");
}

TEST_CASE("atoms parse into their parts") {
    const auto atom = parsed(">=dev-libs/foo-bar-1.2_rc1-r3:1/1.2=::gentoo[a,-b(+),c(-)]");
    CHECK(atom.op == Operator::greater_equal);
    CHECK(atom.cp == "dev-libs/foo-bar");
    CHECK(atom.version.value_or(egraph::Version{}).text == "1.2_rc1-r3");
    CHECK(atom.slot == "1");
    CHECK(atom.sub_slot == "1.2");
    CHECK(atom.repo == "gentoo");
    REQUIRE(atom.use.size() == 3);
    CHECK((atom.use.at(1).flag == "b" && !atom.use.at(1).enabled &&
           atom.use.at(1).fallback == UseDependency::Default::enabled));

    CHECK(parsed("=cat/foo-1-r1*").op == Operator::glob);
    CHECK(parsed("~cat/foo-1.0-r1").op == Operator::approximately);
    CHECK_FALSE(parsed("cat/foo:=").slot.has_value());
    CHECK_FALSE(parsed("cat/foo:*").slot.has_value());
    CHECK(parsed("cat/foo:2=").slot == "2");
    CHECK(parsed("cat/foo-bar").cp == "cat/foo-bar");

    CHECK(atom.slot_operator);
    CHECK(parsed("cat/foo:=").slot_operator);
    CHECK(parsed("cat/foo:2=").slot_operator);
    CHECK_FALSE(parsed("cat/foo:2/2.1").slot_operator);
    CHECK_FALSE(parsed("cat/foo:*").slot_operator);
}

// Every one of these is an InvalidAtom to portage too, except the two that only a parent
// package gives meaning to.
TEST_CASE("invalid atoms are rejected with a reason") {
    using Catch::Matchers::EndsWith;
    const std::vector<std::pair<std::string_view, std::string_view>> cases{
        {"cat/foo-1.0", "a version needs an operator"},
        {"cat/foo-1", "a version needs an operator"},
        {"cat/foo-1a", "a version needs an operator"},
        {">=cat/foo", "an operator needs a version"},
        {"=cat/foo-1*-r1", "an operator needs a version"},
        {">cat/foo-1*", "a '*' version glob needs '='"},
        {"cat/foo[]", "empty USE dependency"},
        {"cat/foo:", "bad slot"},
        {"foo", "no category"},
        {"cat/foo[bar?]", "conditional USE dependencies need a parent package"},
        {"cat/foo[bar=]", "conditional USE dependencies need a parent package"},
        {"!cat/foo", "a blocker is not a query"},
    };
    for (const auto& [text, why] : cases) {
        INFO(text);
        const auto atom = egraph::parse_atom(text);
        REQUIRE_FALSE(atom.has_value());
        CHECK_THAT(atom.error(), EndsWith(std::string{why}));
    }
}

// The evaluated sample's candidates: app-misc/a-1 (USE and IUSE flag), app-misc/a-2 (IUSE flag,
// masked) and dev-libs/b-2, all in slot 0/0 of test_repo. The profile's IUSE_EFFECTIVE is amd64
// and elibc_glibc.
TEST_CASE("ebuilds match with the USE they would be built with") {
    const auto installed = decoded(egraph::test::assemble(egraph::test::sample_sections()));
    const auto evaluated =
        decoded_evaluated(egraph::test::assemble_evaluated(egraph::test::evaluated_sections()));
    const auto matched = [&](std::string_view text) {
        std::vector<std::string> found;
        for (const auto& candidate : evaluated.candidates) {
            if (egraph::matches(installed, evaluated, candidate, parsed(text))) {
                found.emplace_back(evaluated.string(candidate.cpv));
            }
        }
        return found;
    };
    using Found = std::vector<std::string>;
    CHECK(matched("app-misc/a") == Found{"app-misc/a-1", "app-misc/a-2"});
    CHECK(matched(">=app-misc/a-2") == Found{"app-misc/a-2"});
    CHECK(matched("<app-misc/a-2:0/0::test_repo") == Found{"app-misc/a-1"});
    CHECK(matched("app-misc/a:0/1").empty());
    CHECK(matched("app-misc/a::other").empty());
    CHECK(matched("app-misc/a[flag]") == Found{"app-misc/a-1"});
    CHECK(matched("app-misc/a[-flag]") == Found{"app-misc/a-2"});
    // Not in IUSE: only a default decides.
    CHECK(matched("dev-libs/b[flag]").empty());
    CHECK(matched("dev-libs/b[flag(+)]") == Found{"dev-libs/b-2"});
    CHECK(matched("dev-libs/b[-flag(+)]").empty());
    CHECK(matched("dev-libs/b[-flag(-)]") == Found{"dev-libs/b-2"});
    // Implicit, and off.
    CHECK(matched("dev-libs/b[amd64(+)]").empty());
    CHECK(matched("dev-libs/b[-amd64]") == Found{"dev-libs/b-2"});
    CHECK(matched("dev-libs/b[elibc_musl(-)]").empty());
}

namespace {

egraph::Atom config_parsed(std::string_view text) {
    auto atom = egraph::parse_config_atom(text);
    REQUIRE(atom.has_value());
    if (!atom) {
        return {};
    }
    return std::move(*atom);
}

bool config_matches(std::string_view text, std::string_view cp, std::string_view version,
                    std::string_view slot = "0", std::string_view repo = "gentoo") {
    const auto parsed_version = egraph::parse_version(version);
    REQUIRE(parsed_version.has_value());
    return egraph::matches(config_parsed(text), cp, parsed_version.value_or(egraph::Version{}),
                           slot, slot, repo);
}

// The atoms by_specificity puts first to last for cat/pkg-2 in slot 0 of gentoo.
std::vector<std::string_view> specificity(const std::vector<std::string_view>& texts) {
    std::vector<egraph::Atom> atoms;
    for (const auto text : texts) {
        atoms.push_back(config_parsed(text));
    }
    const auto version = egraph::parse_version("2");
    REQUIRE(version.has_value());
    std::vector<std::string_view> order;
    for (const auto index :
         egraph::by_specificity(atoms, "cat/pkg", *version, "0", "0", "gentoo")) {
        order.push_back(texts.at(index));
    }
    return order;
}

} // namespace

TEST_CASE("configuration atoms read portage's extended syntax") {
    CHECK_FALSE(config_parsed("cat/pkg").extended);
    for (const auto* text : {"*/*", "dev-*/*", "*/foo", "dev-lang/rust*", "+*/x"}) {
        INFO(text);
        const auto atom = config_parsed(text);
        CHECK(atom.extended);
        CHECK(atom.cp == text);
    }
    const auto slotted = config_parsed("*/*:0::gentoo");
    CHECK(slotted.cp == "*/*");
    CHECK(slotted.slot == "0");
    CHECK(slotted.repo == "gentoo");
    const auto star = config_parsed("=cat/pkg-*2_rc*");
    CHECK(star.extended);
    CHECK(star.cp == "cat/pkg");
    CHECK(star.contains == "2_rc");

    // Each an InvalidAtom to portage as well, but cat/pkg[a], which its loaders refuse
    // (allow_use=False).
    for (const auto* text :
         {"*/foo-1.0", "c**/p", "*/*[a]", "=*/*-**", "cat/pkg[a]", "*", ">=cat/*-1", "*/*:"}) {
        INFO(text);
        CHECK_FALSE(egraph::parse_config_atom(text).has_value());
    }
}

TEST_CASE("configuration atoms match as match_from_list does") {
    CHECK(config_matches("*/*", "cat/pkg", "1"));
    CHECK(config_matches("cat/*", "cat/pkg", "1"));
    CHECK_FALSE(config_matches("cat/*", "dog/pkg", "1"));
    CHECK(config_matches("dev-*/*", "dev-libs/a", "1"));
    CHECK(config_matches("*/p*g", "cat/pkg", "1"));
    CHECK_FALSE(config_matches("*/p*g", "cat/pkgs", "1"));
    CHECK(config_matches("*/*:0", "cat/pkg", "1"));
    CHECK_FALSE(config_matches("*/*:1", "cat/pkg", "1"));
    CHECK_FALSE(config_matches("*/*::other", "cat/pkg", "1"));
    CHECK(config_matches("=cat/pkg-*2_r*", "cat/pkg", "1.2_rc1"));
    CHECK(config_matches("=cat/pkg-*r1*", "cat/pkg", "1.0-r1"));
    CHECK_FALSE(config_matches("=cat/pkg-*3*", "cat/pkg", "1.2"));
}

// Expectations are portage's ordered_by_atom_specificity for cat/pkg-2:0::gentoo.
TEST_CASE("configuration atoms apply in portage's order of specificity") {
    using V = std::vector<std::string_view>;
    CHECK(specificity({"*/*", "cat/pkg", "cat/pkg:0", ">=cat/pkg-1", "<cat/pkg-3", "=cat/pkg-2",
                       "~cat/pkg-2", "=cat/pkg-2*", "cat/*:0", "=cat/pkg-*2*", "cat/*",
                       "*/*::gentoo"}) ==
          V{"*/*::gentoo", "cat/*", "*/*", "cat/*:0", "=cat/pkg-*2*", "cat/pkg", "<cat/pkg-3",
            ">=cat/pkg-1", "cat/pkg:0", "=cat/pkg-2*", "~cat/pkg-2", "=cat/pkg-2"});
    CHECK(specificity({">=cat/pkg-1", "<cat/pkg-3"}) == V{"<cat/pkg-3", ">=cat/pkg-1"});
    CHECK(specificity({"<cat/pkg-3", ">=cat/pkg-1"}) == V{">=cat/pkg-1", "<cat/pkg-3"});
    CHECK(specificity({">=cat/pkg-1", ">=cat/pkg-2"}) == V{">=cat/pkg-1", ">=cat/pkg-2"});
    CHECK(specificity({">=cat/pkg-1", "<=cat/pkg-5", ">cat/pkg-1.5"}) ==
          V{"<=cat/pkg-5", ">=cat/pkg-1", ">cat/pkg-1.5"});
    CHECK(specificity({"cat/*", "*/*", "*/pkg"}) == V{"*/pkg", "*/*", "cat/*"});
    CHECK(specificity({">=cat/pkg-1:0", "cat/pkg"}) == V{"cat/pkg", ">=cat/pkg-1:0"});
    CHECK(specificity({"dog/pkg", ">cat/pkg-2"}).empty());
}
