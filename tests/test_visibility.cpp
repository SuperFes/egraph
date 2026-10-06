#include "visibility.hpp"

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
using Strings = std::vector<std::string>;

namespace {

std::vector<std::string> words(std::string_view text) {
    std::istringstream in{std::string{text}};
    std::vector<std::string> found;
    for (std::string word; in >> word;) {
        found.push_back(word);
    }
    return found;
}

struct VersionSpec {
    std::string cpv;
    std::string keywords = "x86";
    std::string slot = "0";
    std::string license = {};
    std::string properties = {};
    std::string restrict = {};
    std::string use = {};
    std::string eapi = "8";
    std::string repo = "gentoo";
};

// A repository index on x86, ACCEPT_KEYWORDS="x86" and everything else accepted unless a test
// says otherwise.
class IndexBuilder {
  public:
    IndexBuilder() {
        intern("");
        for (const auto* name : {"gentoo", "overlay"}) {
            index_.repositories.push_back({.name = intern(name), .location = intern("/")});
        }
        auto& vis = index_.visibility;
        vis.accept_keywords = ids("x86");
        vis.arch = intern("x86");
        for (const auto* eapi : {"7", "8"}) {
            vis.eapis.push_back({.eapi = intern(eapi), .supported = true, .deprecated = false});
        }
        vis.eapis.push_back({.eapi = intern("6"), .supported = true, .deprecated = true});
        vis.eapis.push_back({.eapi = intern("99"), .supported = false, .deprecated = false});
        vis.accept_license = ids("*");
        vis.accept_properties = ids("*");
        vis.accept_restrict = ids("*");
    }

    std::uint32_t version(const VersionSpec& spec) {
        egraph::IndexVersion version;
        const auto dash = spec.cpv.rfind('-', spec.cpv.find_last_of("0123456789") - 1);
        version.cp = intern(spec.cpv.substr(0, dash));
        version.cpv = intern(spec.cpv);
        const auto slash = spec.slot.find('/');
        version.slot = intern(spec.slot.substr(0, slash));
        version.sub_slot =
            intern(slash == std::string::npos ? spec.slot : spec.slot.substr(slash + 1));
        version.eapi = intern(spec.eapi);
        version.repository = spec.repo == "gentoo" ? 0 : 1;
        version.keywords = ids(spec.keywords);
        version.license = ids(spec.license);
        version.properties = ids(spec.properties);
        version.restrict = ids(spec.restrict);
        version.use = ids(spec.use);
        index_.versions.push_back(version);
        return static_cast<std::uint32_t>(index_.versions.size() - 1);
    }

    Range entries(const std::vector<std::pair<std::string, std::string>>& lines) {
        const auto first = static_cast<std::uint32_t>(index_.entries.size());
        for (const auto& [atom, tokens] : lines) {
            index_.entries.push_back({.atom = intern(atom), .tokens = ids(tokens)});
        }
        return {.first = first, .count = static_cast<std::uint32_t>(index_.entries.size()) - first};
    }

    Range ids(std::string_view text) {
        const auto first = static_cast<std::uint32_t>(index_.ids.size());
        for (const auto& word : words(text)) {
            index_.ids.push_back(intern(word));
        }
        return {.first = first, .count = static_cast<std::uint32_t>(index_.ids.size()) - first};
    }

    egraph::VisibilityConfig& config() { return index_.visibility; }
    [[nodiscard]] const RepositoryIndex& index() const { return index_; }

  private:
    std::uint32_t intern(std::string_view text) {
        const auto [found, added] =
            ids_.emplace(std::string{text}, static_cast<std::uint32_t>(index_.strings.size()));
        if (added) {
            index_.strings.push_back({.first = static_cast<std::uint32_t>(index_.pool.size()),
                                      .count = static_cast<std::uint32_t>(text.size())});
            index_.pool += text;
        }
        return found->second;
    }

    RepositoryIndex index_;
    std::map<std::string, std::uint32_t, std::less<>> ids_;
};

} // namespace

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
