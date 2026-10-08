#include "visibility_stack.hpp"

#include "index_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using egraph::LedgerEntry;
using egraph::SourcedToken;
using egraph::stack_visibility;
using egraph::StackedKey;
using egraph::StackedMask;
using egraph::test::IndexBuilder;
using egraph::test::LedgerLine;
using Strings = std::vector<std::string>;

namespace {

Strings atoms_of(const std::vector<StackedMask>& masks) {
    return masks | std::views::transform(&StackedMask::atom) | std::ranges::to<std::vector>();
}

Strings tokens_of(const std::vector<SourcedToken>& tokens) {
    return tokens | std::views::transform(&SourcedToken::token) | std::ranges::to<std::vector>();
}

std::vector<std::pair<std::string, Strings>> keys_of(const std::vector<StackedKey>& keys) {
    return keys | std::views::transform([](const StackedKey& key) {
               return std::pair{key.atom, tokens_of(key.tokens)};
           }) |
           std::ranges::to<std::vector>();
}

// Which line of the test an entry is, by its line number.
std::uint32_t line_of(const IndexBuilder& b, std::optional<std::uint32_t> entry) {
    return entry ? b.index().ledger_entries.at(*entry).line : 0;
}

} // namespace

TEST_CASE("visibility stack: masks over masters, with ::repo, then the profiles' and the user's") {
    IndexBuilder b;
    auto& ledger = b.ledger();
    ledger.repositories.push_back(
        {.name = b.string("gentoo"),
         .masters = {},
         .package_mask = b.lines({{.atom = "app-misc/a", .line = 1},
                                  {.atom = "app-misc/b", .line = 2},
                                  {.atom = "app-misc/c::gentoo", .line = 3}}),
         .package_unmask = {}});
    ledger.repositories.push_back({.name = b.string("overlay"),
                                   .masters = b.ids("gentoo"),
                                   .package_mask = b.lines({{.atom = "-app-misc/a", .line = 4},
                                                            {.atom = "app-misc/d", .line = 5}}),
                                   .package_unmask = {}});
    ledger.profiles.push_back({.path = b.string("/profile"),
                               .defaults = {},
                               .package_mask = b.lines({{.atom = "app-misc/e", .line = 6},
                                                        {.atom = "app-misc/b", .line = 7}}),
                               .package_unmask = b.lines({{.atom = "app-misc/b", .line = 8}}),
                               .package_keywords = {},
                               .package_accept_keywords = {},
                               .package_license = {}});
    ledger.package_mask = b.lines({{.atom = "*/*::overlay", .line = 9},
                                   {.atom = "-app-misc/d", .line = 10},
                                   {.atom = "app-misc/f", .line = 11}});
    const auto stacked = stack_visibility(b.index());
    // gentoo's own, then overlay's stacked over gentoo's: its removal holds for overlay alone.
    // The user's -app-misc/d removes app-misc/d::overlay. By cp, the wildcard last.
    CHECK(atoms_of(stacked.masks) ==
          Strings{"app-misc/a::gentoo", "app-misc/b::gentoo", "app-misc/b::overlay", "app-misc/b",
                  "app-misc/c::gentoo", "app-misc/e", "app-misc/f", "*/*::overlay"});
    CHECK(line_of(b, stacked.masks.at(0).entry) == 1);
    CHECK(line_of(b, stacked.masks.at(3).entry) == 7);
    CHECK(atoms_of(stacked.unmasks) == Strings{"app-misc/b"});
}

TEST_CASE("visibility stack: ACCEPT_ variables over their layers, license groups expanded") {
    IndexBuilder b;
    auto& ledger = b.ledger();
    ledger.globals = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "* -@EULA", .line = 1},
                              {.var = "ACCEPT_KEYWORDS", .tokens = "x86", .line = 2}});
    ledger.profiles.push_back(
        {.path = b.string("/profile"),
         .defaults = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "amd64 -x86", .line = 3},
                              {.var = "ACCEPT_PROPERTIES", .tokens = "-* live", .line = 4}}),
         .package_mask = {},
         .package_unmask = {},
         .package_keywords = {},
         .package_accept_keywords = {},
         .package_license = {}});
    ledger.conf =
        b.lines({{.var = "ACCEPT_LICENSE", .tokens = "-* @FREE", .line = 5},
                 {.var = "ACCEPT_KEYWORDS", .tokens = "~amd64 amd64", .line = 6},
                 {.atom = "*/*", .var = "ACCEPT_LICENSE", .tokens = "-@MINE", .line = 7}});
    ledger.env = b.lines({{.var = "ACCEPT_PROPERTIES", .tokens = "interactive", .line = 8}});
    ledger.license_groups = b.lines({{.var = "FREE", .tokens = "GPL @OSI", .line = 9},
                                     {.var = "OSI", .tokens = "MIT", .line = 10},
                                     {.var = "MINE", .tokens = "EULA", .line = 11},
                                     {.var = "FREE", .tokens = "BSD", .line = 12}});
    const auto stacked = stack_visibility(b.index());
    CHECK(tokens_of(stacked.accept_keywords) == Strings{"amd64", "~amd64"});
    // Each the entry that added it last.
    CHECK(line_of(b, stacked.accept_keywords.at(0).entry) == 6);
    // Pruned after the last -*; a group's members in order.
    CHECK(tokens_of(stacked.accept_license) == Strings{"MIT", "BSD", "GPL", "-EULA"});
    CHECK(line_of(b, stacked.accept_license.at(0).entry) == 5);
    CHECK(line_of(b, stacked.accept_license.at(3).entry) == 7);
    // What refuses a license nothing names.
    CHECK(line_of(b, stacked.license_cleared) == 5);
    CHECK(tokens_of(stacked.accept_properties) == Strings{"live", "interactive"});
    CHECK(line_of(b, stacked.properties_cleared) == 4);
    CHECK_FALSE(stacked.restrict_cleared);
    CHECK(stacked.accept_restrict.empty());
}

TEST_CASE("visibility stack: without ACCEPT_LICENSE, portage's own, from no entry") {
    IndexBuilder b;
    b.ledger().globals = {};
    b.ledger().license_groups = b.lines({{.var = "EULA", .tokens = "a b"}});
    const auto stacked = stack_visibility(b.index());
    CHECK(tokens_of(stacked.accept_license) == Strings{"*", "-a", "-b"});
    CHECK_FALSE(stacked.accept_license.front().entry);
}

TEST_CASE("visibility stack: a group met again or undefined stays as it is") {
    IndexBuilder b;
    auto& ledger = b.ledger();
    ledger.globals = {};
    ledger.conf = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "@LOOP @NONE"}});
    ledger.license_groups = b.lines({{.var = "LOOP", .tokens = "A @LOOP -B"}});
    CHECK(tokens_of(stack_visibility(b.index()).accept_license) == Strings{"@LOOP", "A", "@NONE"});
}

TEST_CASE("visibility stack: package.* lines for one atom join within a source, and a later "
          "source replaces them") {
    IndexBuilder b;
    auto& ledger = b.ledger();
    ledger.profiles.push_back(
        {.path = b.string("/profile"),
         .defaults = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "x86 ~arm -amd64"}}),
         .package_mask = {},
         .package_unmask = {},
         .package_keywords = {},
         .package_accept_keywords = {},
         .package_license = b.lines(
             {{.atom = "app-misc/a", .tokens = "A"}, {.atom = "app-misc/b", .tokens = "B"}})});
    ledger.package_license = b.lines({{.atom = "app-misc/*", .tokens = "W"},
                                      {.atom = "app-misc/b", .tokens = "C"},
                                      {.atom = "app-misc/b", .tokens = "D"}});
    ledger.package_keywords = b.lines({{.atom = "app-misc/k", .tokens = "**"}});
    ledger.package_accept_keywords = b.lines(
        {{.atom = "app-misc/k", .tokens = "~x86"}, {.atom = "app-misc/empty", .tokens = ""}});
    const auto stacked = stack_visibility(b.index());
    using Keys = std::vector<std::pair<std::string, Strings>>;
    CHECK(keys_of(stacked.licenses) ==
          Keys{{"app-misc/a", {"A"}}, {"app-misc/b", {"C", "D"}}, {"app-misc/*", {"W"}}});
    CHECK(stacked.licenses.at(1).entries.size() == 2);
    CHECK(keys_of(stacked.accept_keywords_entries) ==
          Keys{{"app-misc/k", {"**", "~x86"}}, {"app-misc/empty", {"~x86"}}});
}
