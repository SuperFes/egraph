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

using egraph::Range;
using egraph::RepositoryIndex;
using egraph::VersionMasks;
using egraph::test::IndexBuilder;
using Strings = std::vector<std::string>;

namespace {} // namespace

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
    b.config().accept_keywords = b.ids("x86 ~*");
    {
        const VersionMasks masking{b.index()};
        CHECK(masking.visible(testing));
        CHECK_FALSE(masking.visible(stable));
        CHECK_FALSE(masking.visible(none));
    }
    b.config().accept_keywords = b.ids("x86 *");
    {
        const VersionMasks masking{b.index()};
        CHECK_FALSE(masking.visible(testing));
        CHECK(masking.visible(stable));
    }
    b.config().accept_keywords = b.ids("x86 **");
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
    b.config().accept_keywords_entries = b.entries({
        {"<app-misc/ranged-3", "~x86"},
        {">=app-misc/ranged-2", "-~x86"},
        {"=app-misc/ranged-2", "~x86"},
        {"app-misc/slotted:2", "~x86"},
        {"app-misc/anything", "**"},
        {"app-misc/wild*", "~x86"},
        {"*/*::overlay", "~x86"},
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
    b.config().profile_keywords = {b.entries({{"app-misc/keyworded", "x86"}}),
                                   b.entries({{"app-misc/dropped", "-x86"}})};
    // No tokens: the ~ form of each stable ACCEPT_KEYWORDS keyword.
    b.config().profile_accept_keywords = {b.entries({{"app-misc/profiled", ""}})};
    const VersionMasks masking{b.index()};
    CHECK(masking.visible(keyworded));
    CHECK(masking.visible(profiled));
    CHECK_FALSE(masking.visible(unkeyworded));
}

TEST_CASE("keywords: the environment's ACCEPT_KEYWORDS stacks last") {
    IndexBuilder b;
    const auto testing = b.version({.cpv = "app-misc/a-1", .keywords = "~x86"});
    b.config().accept_keywords = b.ids("x86 ~x86");
    b.config().environment_keywords = b.ids("-~x86");
    CHECK_FALSE(VersionMasks{b.index()}.visible(testing));
}

TEST_CASE("package.mask, unless package.unmask matches too") {
    IndexBuilder b;
    const auto masked = b.version({.cpv = "app-misc/masked-1"});
    const auto newer = b.version({.cpv = "app-misc/masked-2"});
    const auto unmasked = b.version({.cpv = "app-misc/unmasked-1"});
    const auto repo = b.version({.cpv = "app-misc/repo-1"});
    const auto elsewhere = b.version({.cpv = "app-misc/repo-1", .repo = "overlay"});
    const auto future = b.version({.cpv = "app-misc/future-1", .eapi = "99"});
    b.config().masks = b.ids(">=app-misc/masked-2 app-misc/unmasked app-misc/repo::gentoo "
                             "app-misc/future");
    b.config().unmasks = b.ids("app-misc/unmasked");
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
    b.config().accept_license = b.ids("* -EULA -TEST");
    b.config().licenses = b.entries({{"app-misc/eula-ok", "EULA"}});
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
    b.config().accept_license = b.ids("-* GPL-2");
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
    b.config().accept_properties = b.ids("* -interactive");
    b.config().accept_restrict = b.ids("* -fetch");
    b.config().restrict = b.entries({{"app-misc/c", "fetch"}});
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
    b.config().masks = b.ids("app-misc/a");
    b.config().accept_license = b.ids("* -EULA");
    b.config().accept_properties = b.ids("* -interactive");
    b.config().accept_restrict = b.ids("* -fetch");
    CHECK(VersionMasks{b.index()}.reasons(all) == Strings{"package.mask", "EULA license(s)",
                                                          "interactive properties",
                                                          "fetch in RESTRICT", "~x86 keyword"});
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
                          "dev-libs/a-10::gentoo\t0/2\tmasked\t~x86 keyword",
                          "net-misc/a-1::gentoo\t0\tvisible", "net-misc/b-1::gentoo\t0\tvisible"});
    const Strings by_name{"a"};
    CHECK(egraph::version_lines(b.index(), masking, by_name)->size() == 4);
    const Strings by_atom{">=dev-libs/a-3"};
    CHECK(*egraph::version_lines(b.index(), masking, by_atom) ==
          Strings{"dev-libs/a-10::gentoo\t0/2\tmasked\t~x86 keyword"});
    const Strings unknown{"dev-libs/c"};
    CHECK(egraph::version_lines(b.index(), masking, unknown).error() ==
          "dev-libs/c: no version in the repositories");
    const Strings use{"dev-libs/a[x]"};
    CHECK_FALSE(egraph::version_lines(b.index(), masking, use));
}
