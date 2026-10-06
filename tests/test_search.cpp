#include "search.hpp"

#include "index_builder.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::WithinULP;
using egraph::SearchOptions;
using Views = std::vector<std::string_view>;
using Strings = std::vector<std::string>;

namespace {

const std::vector<std::string_view> cps{
    "app-misc/foo",     "dev-libs/openssl", "dev-libs/openssl-compat",
    "net-misc/openssh", "sys-apps/portage", "www-client/firefox"};

const std::map<std::string_view, std::string_view> descriptions{
    {"dev-libs/openssl", "Robust, full-featured Open Source Toolkit"},
    {"sys-apps/portage", "The package management system"},
};

std::string_view description(std::string_view cp) {
    const auto found = descriptions.find(cp);
    return found == descriptions.end() ? std::string_view{} : found->second;
}

Views search(std::string_view key, SearchOptions options = {}) {
    auto found = egraph::search_cps(cps, description, key, options);
    REQUIRE(found);
    return *found;
}

} // namespace

TEST_CASE("sequence_ratio is difflib's") {
    CHECK_THAT(egraph::sequence_ratio("openssl", "openssl"), WithinULP(1.0, 0));
    CHECK_THAT(egraph::sequence_ratio("openssl", "opnessl"), WithinULP(0.8571428571428571, 0));
    CHECK_THAT(egraph::sequence_ratio("openssh", "openssl"), WithinULP(0.8571428571428571, 0));
    CHECK_THAT(egraph::sequence_ratio("abcd", "bcda"), WithinULP(0.75, 0));
    CHECK_THAT(egraph::sequence_ratio("qabxcd", "abycdf"), WithinULP(0.6666666666666666, 0));
    CHECK_THAT(egraph::sequence_ratio("", "a"), WithinULP(0.0, 0));
    CHECK_THAT(egraph::sequence_ratio("", ""), WithinULP(1.0, 0));
    CHECK_THAT(egraph::sequence_ratio("aaaa", "aa"), WithinULP(0.6666666666666666, 0));
    CHECK_THAT(egraph::sequence_ratio("ababab", "baba"), WithinULP(0.8, 0));
    CHECK_THAT(egraph::sequence_ratio("libreoffice", "liberoffice"),
               WithinULP(0.9090909090909091, 0));
}

TEST_CASE("a key matches names anywhere, ignoring case, or fuzzily") {
    CHECK(search("SSL") == Views{"dev-libs/openssl", "dev-libs/openssl-compat"});
    CHECK(search("opnessl") == Views{"dev-libs/openssl"});
    CHECK(search("opnessl", {.fuzzy = false}).empty());
    CHECK(search("opnessl", {.similarity = 90}).empty());
    // Names, not categories, unless the key has a category.
    CHECK(search("misc") == Views{});
    CHECK(search("misc/") == Views{"app-misc/foo", "net-misc/openssh"});
    CHECK(search("@app") == Views{"app-misc/foo", "sys-apps/portage"});
}

TEST_CASE("a key can be a regular expression") {
    CHECK(search("%^open") ==
          Views{"dev-libs/openssl", "dev-libs/openssl-compat", "net-misc/openssh"});
    CHECK(search("^open") ==
          Views{"dev-libs/openssl", "dev-libs/openssl-compat", "net-misc/openssh"});
    CHECK(search("ssl$") == Views{"dev-libs/openssl"});
    CHECK(search("^open", {.fuzzy = false, .regex_auto = false}).empty());
    // One that does not compile is text after all, unless asked for with %.
    CHECK(search("fire[").empty());
    CHECK_FALSE(egraph::search_cps(cps, description, "%fire[", {}));
}

TEST_CASE("with -S, descriptions too") {
    CHECK(search("toolkit").empty());
    CHECK(search("toolkit", {.description = true}) == Views{"dev-libs/openssl"});
    CHECK(search("management", {.description = true}) == Views{"sys-apps/portage"});
}

TEST_CASE("search_lines: the best visible version, else the best, with its ebuild's metadata") {
    const auto system = egraph::test::make_system({{.cpv = "dev-libs/openssl-3.4.0"}}, {});
    egraph::test::IndexBuilder b;
    b.version({.cpv = "dev-libs/openssl-3.4.0", .description = "old", .homepage = "h"});
    b.version({.cpv = "dev-libs/openssl-3.5.0", .description = "Toolkit", .homepage = "h"});
    b.version({.cpv = "dev-libs/openssl-4.0", .keywords = "~x86", .description = "new"});
    b.version({.cpv = "dev-libs/openssl-3.5.0",
               .repo = "overlay",
               .description = "Overlay's",
               .homepage = "o"});
    b.version({.cpv = "app-misc/masked-2", .keywords = "~x86", .description = "m"});
    b.version({.cpv = "app-misc/masked-1", .keywords = "~x86", .description = "m1"});
    const egraph::VersionMasks masks{b.index()};
    const Strings keys{"openssl", "masked"};
    const auto lines =
        egraph::search_lines(system.store, system.evaluated, b.index(), masks, keys, {});
    REQUIRE(lines);
    // gentoo, first in the index, has the highest priority.
    CHECK(*lines == Strings{"openssl\tdev-libs/openssl\t3.5.0\tvisible\t3.4.0\th\t\tToolkit",
                            "masked\tapp-misc/masked\t2\tmasked\t\t\t\tm"});
}
