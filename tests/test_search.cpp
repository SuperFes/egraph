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

TEST_CASE("Catalogue::versions: every ebuild and installed package of a cp, lowest first") {
    auto system = egraph::test::make_system(
        {{.cpv = "dev-libs/openssl-3.4.0"}, {.cpv = "dev-libs/openssl-3.5.0"}}, {});
    egraph::test::IndexBuilder b;
    b.version({.cpv = "dev-libs/openssl-4.0", .keywords = "~x86", .slot = "0/4"});
    b.version({.cpv = "dev-libs/openssl-3.5.0", .repo = "overlay"});
    b.version({.cpv = "dev-libs/openssl-3.5.0"});
    const egraph::VersionMasks masks{b.index()};
    const egraph::Catalogue catalogue{system.store, system.evaluated, b.index(), masks};
    const auto versions = catalogue.versions("dev-libs/openssl");
    REQUIRE(versions.size() == 5);
    // 3.4.0 is only installed; the installed 3.5.0 came from test_repo, which the index lacks.
    CHECK(versions.at(0).version == "3.4.0");
    CHECK_FALSE(versions.at(0).ebuild);
    CHECK(versions.at(0).repo == "test_repo");
    CHECK(versions.at(0).installed == 0U);
    CHECK(versions.at(1).repo == "gentoo");
    CHECK(versions.at(2).repo == "overlay");
    CHECK(versions.at(3).repo == "test_repo");
    CHECK(versions.at(3).installed == 1U);
    CHECK(versions.at(4).version == "4.0");
    CHECK(versions.at(4).sub_slot == "4");
    CHECK_FALSE(versions.at(4).visible);
    CHECK(versions.at(4).reasons == Strings{"~x86 keyword (file:1)"});
}

TEST_CASE("Catalogue::versions marks the ebuild an installed package came from") {
    auto system = egraph::test::make_system({{.cpv = "app-misc/foo-1"}}, {});
    egraph::test::IndexBuilder b;
    b.version({.cpv = "app-misc/foo-1", .repo = "overlay"});
    const egraph::VersionMasks masks{b.index()};
    // Installed from test_repo, not from the overlay's ebuild of the same cpv.
    const egraph::Catalogue catalogue{system.store, system.evaluated, b.index(), masks};
    CHECK(catalogue.versions("app-misc/foo").size() == 2);
}

TEST_CASE("Catalogue::found: what a search result shows") {
    const auto system = egraph::test::make_system({{.cpv = "dev-libs/openssl-3.4.0"}}, {});
    egraph::test::IndexBuilder b;
    b.version({.cpv = "dev-libs/openssl-3.5.0", .license = "Apache-2.0", .description = "TLS"});
    const egraph::VersionMasks masks{b.index()};
    const egraph::Catalogue catalogue{system.store, system.evaluated, b.index(), masks};
    const auto found = catalogue.found("dev-libs/openssl");
    CHECK(found.version == "3.5.0");
    CHECK(found.visible);
    CHECK(found.installed == "3.4.0");
    CHECK(found.license == "Apache-2.0");
    CHECK(found.description == "TLS");
    CHECK(catalogue.contains("dev-libs/openssl"));
    CHECK_FALSE(catalogue.contains("dev-libs/libressl"));
}

TEST_CASE("Catalogue::description: an indexed repository's versions that do not parse give none") {
    const auto system = egraph::test::make_system({}, {});
    egraph::test::IndexBuilder b;
    b.describe("gentoo");
    b.version({.cpv = "app-misc/odd-1x.y", .description = "Odd"});
    const egraph::VersionMasks masks{b.index()};
    const egraph::Catalogue catalogue{system.store, system.evaluated, b.index(), masks};
    CHECK(catalogue.description("app-misc/odd").empty());
}
