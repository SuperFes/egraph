#include "visibility.hpp"

#include "index_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using egraph::VersionMasks;
using egraph::test::IndexBuilder;
using Strings = std::vector<std::string>;

TEST_CASE("keywords: stable accepted, testing and missing ones masked as portage words them") {
    IndexBuilder b;
    const auto stable = b.version({.cpv = "app-misc/a-1"});
    const auto testing = b.version({.cpv = "app-misc/b-1", .keywords = "~x86 amd64"});
    const auto none = b.version({.cpv = "app-misc/c-1", .keywords = ""});
    const auto other = b.version({.cpv = "app-misc/d-1", .keywords = "amd64 ~arm"});
    const auto negated = b.version({.cpv = "app-misc/e-1", .keywords = "-x86 amd64"});
    const auto all_but = b.version({.cpv = "app-misc/f-1", .keywords = "-* x86"});
    const VersionMasks masking{b.index()};
    CHECK(masking.visible(stable));
    CHECK(masking.reasons(stable).empty());
    CHECK_FALSE(masking.visible(testing));
    CHECK(masking.reasons(testing) == Strings{"~x86 keyword"});
    CHECK(masking.reasons(none) == Strings{"missing keyword"});
    CHECK(masking.reasons(other) == Strings{"missing keyword"});
    // A negated keyword in KEYWORDS drops out as stack_lists drops it.
    CHECK(masking.reasons(negated) == Strings{"missing keyword"});
    CHECK(masking.visible(all_but));
}

TEST_CASE("keywords: ACCEPT_KEYWORDS' wildcards") {
    IndexBuilder b;
    const auto testing = b.version({.cpv = "app-misc/a-1", .keywords = "~amd64"});
    const auto stable = b.version({.cpv = "app-misc/b-1", .keywords = "amd64"});
    const auto none = b.version({.cpv = "app-misc/c-1", .keywords = ""});
    auto& conf = b.ledger().conf;
    conf = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "~*"}});
    {
        const VersionMasks masking{b.index()};
        CHECK(masking.visible(testing));
        CHECK_FALSE(masking.visible(stable));
        CHECK_FALSE(masking.visible(none));
    }
    conf = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "*"}});
    {
        const VersionMasks masking{b.index()};
        CHECK_FALSE(masking.visible(testing));
        CHECK(masking.visible(stable));
    }
    conf = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "**"}});
    CHECK(VersionMasks{b.index()}.visible(none));
}

TEST_CASE("keywords: package.accept_keywords, the most specific atom last") {
    IndexBuilder b;
    const auto one = b.version({.cpv = "app-misc/ranged-1", .keywords = "~x86"});
    const auto two = b.version({.cpv = "app-misc/ranged-2", .keywords = "~x86"});
    const auto three = b.version({.cpv = "app-misc/ranged-3", .keywords = "~x86"});
    const auto slot1 = b.version({.cpv = "app-misc/slotted-1", .keywords = "~x86", .slot = "1"});
    const auto slot2 = b.version({.cpv = "app-misc/slotted-2", .keywords = "~x86", .slot = "2"});
    const auto anything = b.version({.cpv = "app-misc/anything-1", .keywords = ""});
    const auto wild = b.version({.cpv = "app-misc/wildcard-1", .keywords = "~x86"});
    const auto over = b.version({.cpv = "app-misc/over-1", .keywords = "~x86", .repo = "overlay"});
    const auto gentoo = b.version({.cpv = "app-misc/under-1", .keywords = "~x86"});
    b.ledger().package_accept_keywords = b.lines({
        {.atom = "<app-misc/ranged-3", .tokens = "~x86"},
        {.atom = ">=app-misc/ranged-2", .tokens = "-~x86"},
        {.atom = "=app-misc/ranged-2", .tokens = "~x86"},
        {.atom = "app-misc/slotted:2", .tokens = "~x86"},
        {.atom = "app-misc/anything", .tokens = "**"},
        {.atom = "app-misc/wild*", .tokens = "~x86"},
        {.atom = "*/*::overlay", .tokens = "~x86"},
    });
    const VersionMasks masking{b.index()};
    CHECK(masking.visible(one));
    CHECK(masking.visible(two));
    CHECK_FALSE(masking.visible(three));
    CHECK(masking.reasons(three) == Strings{"~x86 keyword"});
    CHECK_FALSE(masking.visible(slot1));
    CHECK(masking.visible(slot2));
    CHECK(masking.visible(anything));
    CHECK(masking.visible(wild));
    CHECK(masking.visible(over));
    CHECK_FALSE(masking.visible(gentoo));
}

TEST_CASE("keywords: the profiles' package.keywords and package.accept_keywords") {
    IndexBuilder b;
    const auto keyworded = b.version({.cpv = "app-misc/keyworded-1", .keywords = "amd64"});
    const auto profiled = b.version({.cpv = "app-misc/profiled-1", .keywords = "~x86"});
    const auto unkeyworded = b.version({.cpv = "app-misc/dropped-1", .keywords = "x86"});
    b.profile().package_keywords = b.lines({{.atom = "app-misc/keyworded", .tokens = "x86"}});
    // No tokens: the ~ form of each stable ACCEPT_KEYWORDS keyword.
    b.ledger().profiles.back().package_accept_keywords =
        b.lines({{.atom = "app-misc/profiled", .tokens = ""}});
    b.profile().package_keywords = b.lines({{.atom = "app-misc/dropped", .tokens = "-x86"}});
    const VersionMasks masking{b.index()};
    CHECK(masking.visible(keyworded));
    CHECK(masking.visible(profiled));
    CHECK_FALSE(masking.visible(unkeyworded));
}

TEST_CASE("keywords: the environment's ACCEPT_KEYWORDS stacks last") {
    IndexBuilder b;
    const auto testing = b.version({.cpv = "app-misc/a-1", .keywords = "~x86"});
    b.ledger().conf = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "~x86"}});
    b.ledger().env = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "-~x86"}});
    CHECK_FALSE(VersionMasks{b.index()}.visible(testing));
}

TEST_CASE("keywords: a mask names ARCH's keyword, or else the first accepted one's, sorted") {
    IndexBuilder b;
    const auto testing = b.version({.cpv = "app-misc/a-1", .keywords = "~arm"});
    // settings["ACCEPT_KEYWORDS"] is sorted: arm before ~sparc.
    b.ledger().conf = b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "-x86 ~sparc arm"}});
    CHECK(VersionMasks{b.index()}.reasons(testing) == Strings{"~arm keyword"});
}

TEST_CASE("package.mask: a repository's own carries its ::repo, and the user's -atom removes it") {
    IndexBuilder b;
    const auto gentoo = b.version({.cpv = "app-misc/a-1"});
    const auto overlay = b.version({.cpv = "app-misc/a-1", .repo = "overlay"});
    const auto kept = b.version({.cpv = "app-misc/b-1"});
    b.ledger().repositories.push_back(
        {.name = b.string("gentoo"),
         .masters = {},
         .package_mask = b.lines({{.atom = "app-misc/a"}, {.atom = "app-misc/b"}}),
         .package_unmask = {}});
    b.ledger().package_mask = b.lines({{.atom = "-app-misc/b"}});
    const VersionMasks masking{b.index()};
    CHECK(masking.reasons(gentoo) == Strings{"package.mask"});
    CHECK(masking.visible(overlay));
    CHECK(masking.visible(kept));
}

TEST_CASE("package.mask, unless package.unmask matches too") {
    IndexBuilder b;
    const auto masked = b.version({.cpv = "app-misc/masked-1"});
    const auto newer = b.version({.cpv = "app-misc/masked-2"});
    const auto unmasked = b.version({.cpv = "app-misc/unmasked-1"});
    const auto repo = b.version({.cpv = "app-misc/repo-1"});
    const auto elsewhere = b.version({.cpv = "app-misc/repo-1", .repo = "overlay"});
    const auto future = b.version({.cpv = "app-misc/future-1", .eapi = "99"});
    b.ledger().package_mask = b.lines({{.atom = ">=app-misc/masked-2"},
                                       {.atom = "app-misc/unmasked"},
                                       {.atom = "app-misc/repo::gentoo"},
                                       {.atom = "app-misc/future"}});
    b.ledger().package_unmask = b.lines({{.atom = "app-misc/unmasked"}});
    const VersionMasks masking{b.index()};
    CHECK(masking.visible(masked));
    CHECK(masking.reasons(newer) == Strings{"package.mask"});
    CHECK(masking.visible(unmasked));
    CHECK_FALSE(masking.visible(repo));
    CHECK(masking.visible(elsewhere));
    // An EAPI portage cannot read is the only reason given.
    CHECK_FALSE(masking.visible(future));
    CHECK(masking.reasons(future) == Strings{"EAPI 99"});
}

TEST_CASE("EAPIs portage does not support or has deprecated, and an empty SLOT, mask") {
    IndexBuilder b;
    const auto old = b.version({.cpv = "app-misc/a-1", .eapi = "6"});
    const auto unslotted = b.version({.cpv = "app-misc/b-1", .slot = ""});
    const VersionMasks masking{b.index()};
    CHECK_FALSE(masking.visible(old));
    CHECK(masking.reasons(old) == Strings{"EAPI 6"});
    CHECK_FALSE(masking.visible(unslotted));
    CHECK(masking.reasons(unslotted).back() == "SLOT: undefined");
}

TEST_CASE("licenses: refused, accepted per package, in || and under conditionals") {
    IndexBuilder b;
    const auto eula = b.version({.cpv = "app-misc/eula-1", .license = "EULA"});
    const auto eula_ok = b.version({.cpv = "app-misc/eula-ok-1", .license = "EULA"});
    const auto either = b.version({.cpv = "app-misc/either-1", .license = "|| ( EULA GPL-2 )"});
    const auto neither = b.version({.cpv = "app-misc/neither-1", .license = "|| ( EULA TEST )"});
    const auto on =
        b.version({.cpv = "app-misc/on-1", .license = "GPL-2 bin? ( EULA )", .use = "bin"});
    const auto off = b.version({.cpv = "app-misc/off-1", .license = "GPL-2 bin? ( EULA )"});
    const auto negated = b.version({.cpv = "app-misc/neg-1", .license = "!bin? ( EULA )"});
    const auto grouped =
        b.version({.cpv = "app-misc/grouped-1", .license = "|| ( ( EULA GPL-2 ) MIT )"});
    b.ledger().conf = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "* -EULA -TEST"}});
    b.ledger().package_license = b.lines({{.atom = "app-misc/eula-ok", .tokens = "EULA"}});
    const VersionMasks masking{b.index()};
    CHECK(masking.reasons(eula) == Strings{"EULA license(s)"});
    CHECK(masking.visible(eula_ok));
    CHECK(masking.visible(either));
    CHECK(masking.reasons(neither) == Strings{"|| ( EULA TEST ) license(s)"});
    CHECK(masking.reasons(on) == Strings{"( EULA ) license(s)"});
    CHECK(masking.visible(off));
    CHECK_FALSE(masking.visible(negated));
    CHECK(masking.visible(grouped));
}

TEST_CASE("licenses: -* refuses all but what follows") {
    IndexBuilder b;
    const auto gpl = b.version({.cpv = "app-misc/a-1", .license = "GPL-2"});
    const auto mit = b.version({.cpv = "app-misc/b-1", .license = "MIT"});
    b.ledger().conf = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "-* GPL-2"}});
    const VersionMasks masking{b.index()};
    CHECK(masking.visible(gpl));
    CHECK(masking.reasons(mit) == Strings{"MIT license(s)"});
}

TEST_CASE("properties and restrictions refused, and accepted per package") {
    IndexBuilder b;
    const auto interactive = b.version({.cpv = "app-misc/a-1", .properties = "interactive"});
    const auto fetch = b.version({.cpv = "app-misc/b-1", .restrict = "fetch mirror"});
    const auto fetch_ok = b.version({.cpv = "app-misc/c-1", .restrict = "fetch"});
    const auto conditional = b.version({.cpv = "app-misc/d-1", .restrict = "test? ( fetch )"});
    b.ledger().conf = b.lines({{.var = "ACCEPT_PROPERTIES", .tokens = "-interactive"},
                               {.var = "ACCEPT_RESTRICT", .tokens = "-fetch"}});
    b.ledger().package_accept_restrict = b.lines({{.atom = "app-misc/c", .tokens = "fetch"}});
    const VersionMasks masking{b.index()};
    CHECK(masking.reasons(interactive) == Strings{"interactive properties"});
    CHECK(masking.reasons(fetch) == Strings{"fetch in RESTRICT"});
    CHECK(masking.visible(fetch_ok));
    // Without a LICENSE or PROPERTIES conditional portage reads RESTRICT's under no USE.
    CHECK(masking.visible(conditional));
}

TEST_CASE("reasons come in getmaskingstatus's order") {
    IndexBuilder b;
    const auto all = b.version({.cpv = "app-misc/a-1",
                                .keywords = "~x86",
                                .license = "EULA",
                                .properties = "interactive",
                                .restrict = "fetch"});
    b.ledger().package_mask = b.lines({{.atom = "app-misc/a"}});
    b.ledger().conf = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "-EULA"},
                               {.var = "ACCEPT_PROPERTIES", .tokens = "-interactive"},
                               {.var = "ACCEPT_RESTRICT", .tokens = "-fetch"}});
    CHECK(VersionMasks{b.index()}.reasons(all) == Strings{"package.mask", "EULA license(s)",
                                                          "interactive properties",
                                                          "fetch in RESTRICT", "~x86 keyword"});
}

TEST_CASE("each reason shows the lines that decide it") {
    IndexBuilder b;
    const auto all = b.version({.cpv = "app-misc/a-1",
                                .keywords = "~x86",
                                .license = "EULA",
                                .properties = "interactive",
                                .restrict = "fetch"});
    const auto either = b.version({.cpv = "app-misc/b-1", .license = "|| ( EULA TEST )"});
    const auto per_package = b.version({.cpv = "app-misc/c-1", .license = "MIT"});
    const auto missing = b.version({.cpv = "app-misc/d-1", .keywords = ""});
    const auto environment = b.version({.cpv = "app-misc/e-1", .license = "GPL-2"});
    const auto unnumbered = b.version({.cpv = "app-misc/f-1", .license = "BSD"});
    auto& ledger = b.ledger();
    const std::string defaults = "/profile/make.defaults";
    const std::string conf = "/etc/portage/make.conf";
    ledger.globals =
        b.lines({{.var = "ACCEPT_KEYWORDS", .tokens = "x86", .file = defaults, .line = 3},
                 {.var = "ACCEPT_LICENSE", .tokens = "*", .file = defaults, .line = 4},
                 {.var = "ACCEPT_PROPERTIES", .tokens = "*", .file = defaults, .line = 5},
                 {.var = "ACCEPT_RESTRICT", .tokens = "*", .file = defaults, .line = 6}});
    ledger.conf =
        b.lines({{.var = "ACCEPT_LICENSE", .tokens = "-* MIT GPL-2", .file = conf, .line = 4},
                 {.var = "ACCEPT_PROPERTIES", .tokens = "-interactive", .file = conf, .line = 5},
                 {.var = "ACCEPT_RESTRICT", .tokens = "-fetch", .file = conf, .line = 6},
                 {.var = "ACCEPT_LICENSE", .tokens = "BSD -BSD", .file = conf, .line = 0}});
    ledger.env = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "-GPL-2", .file = ""}});
    const auto profile_mask =
        b.lines({{.atom = "app-misc/a", .file = "/profile/package.mask", .line = 9}});
    b.profile().package_mask = profile_mask;
    ledger.package_mask =
        b.lines({{.atom = "app-misc/a", .file = "/etc/portage/package.mask", .line = 2}});
    ledger.package_license = b.lines({{.atom = "app-misc/c",
                                       .tokens = "-MIT",
                                       .file = "/etc/portage/package.license",
                                       .line = 7}});
    const VersionMasks masking{b.index()};
    const auto shown = [&](std::uint32_t id) {
        Strings found;
        for (const auto& reason : masking.sourced_reasons(id)) {
            found.push_back(egraph::shown_reason(b.index(), reason));
        }
        return found;
    };
    // package.mask names the last line listing the atom, as getmaskingreason finds it.
    CHECK(shown(all) == Strings{"package.mask (/etc/portage/package.mask:2)",
                                "EULA license(s) (/etc/portage/make.conf:4)",
                                "interactive properties (/etc/portage/make.conf:5)",
                                "fetch in RESTRICT (/etc/portage/make.conf:6)",
                                "~x86 keyword (/profile/make.defaults:3)"});
    CHECK(shown(either) == Strings{"|| ( EULA TEST ) license(s) (/etc/portage/make.conf:4)"});
    CHECK(shown(per_package) == Strings{"MIT license(s) (/etc/portage/package.license:7)"});
    CHECK(shown(missing) == Strings{"missing keyword"});
    CHECK(shown(environment) == Strings{"GPL-2 license(s)"});
    CHECK(shown(unnumbered) == Strings{"BSD license(s) (/etc/portage/make.conf)"});
    CHECK(masking.reasons(all).front() == "package.mask");
}

TEST_CASE("an unbalanced LICENSE masks") {
    IndexBuilder b;
    const auto broken = b.version({.cpv = "app-misc/a-1", .license = "|| ( GPL-2"});
    const VersionMasks masking{b.index()};
    CHECK_FALSE(masking.visible(broken));
    REQUIRE(masking.reasons(broken).size() == 1);
    CHECK(masking.reasons(broken).front().starts_with("LICENSE: "));
}

TEST_CASE("version_lines: by cp, version and repository, each named package's or all") {
    IndexBuilder b;
    b.version({.cpv = "dev-libs/a-2", .repo = "overlay"});
    b.version({.cpv = "dev-libs/a-10", .keywords = "~x86", .slot = "0/2"});
    b.version({.cpv = "dev-libs/a-2"});
    b.version({.cpv = "net-misc/a-1"});
    b.version({.cpv = "net-misc/b-1"});
    const VersionMasks masking{b.index()};
    const auto all = egraph::version_lines(b.index(), masking, {});
    REQUIRE(all);
    CHECK(*all == Strings{"dev-libs/a-2::gentoo\t0\tvisible", "dev-libs/a-2::overlay\t0\tvisible",
                          "dev-libs/a-10::gentoo\t0/2\tmasked\t~x86 keyword (file:1)",
                          "net-misc/a-1::gentoo\t0\tvisible", "net-misc/b-1::gentoo\t0\tvisible"});
    const Strings by_name{"a"};
    CHECK(egraph::version_lines(b.index(), masking, by_name)->size() == 4);
    const Strings by_atom{">=dev-libs/a-3"};
    CHECK(*egraph::version_lines(b.index(), masking, by_atom) ==
          Strings{"dev-libs/a-10::gentoo\t0/2\tmasked\t~x86 keyword (file:1)"});
    const Strings unknown{"dev-libs/c"};
    CHECK(egraph::version_lines(b.index(), masking, unknown).error() ==
          "dev-libs/c: no version in the repositories");
    const Strings use{"dev-libs/a[x]"};
    CHECK_FALSE(egraph::version_lines(b.index(), masking, use));
}
