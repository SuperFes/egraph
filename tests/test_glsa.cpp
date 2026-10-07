#include "glsa.hpp"
#include "helpers.hpp"
#include "index_builder.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using egraph::test::Installed;
using egraph::test::make_system;

namespace {

// Whether the installed cpv is in range, on a system holding only it.
bool in_range(const Installed& installed, std::string_view range) {
    const auto system = make_system({installed}, {});
    REQUIRE(system.store.packages.size() == 1);
    return egraph::in_advisory_range(system.store, system.store.packages.front(), range);
}

std::vector<std::string> ids(const std::vector<egraph::AffectedAdvisory>& found) {
    std::vector<std::string> names;
    names.reserve(found.size());
    for (const auto& advisory : found) {
        names.push_back(advisory.id);
    }
    return names;
}

} // namespace

TEST_CASE("plain GLSA ranges match as vardb.match") {
    CHECK(in_range({.cpv = "dev-libs/a-1.2"}, "<dev-libs/a-1.3"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.3"}, "<dev-libs/a-1.3"));
    CHECK(in_range({.cpv = "dev-libs/a-1.3"}, ">=dev-libs/a-1.3"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/b-1.3"}, ">=dev-libs/a-1.3"));
    CHECK(in_range({.cpv = "dev-libs/a-1.2", .slot = "1"}, "<dev-libs/a-2:1"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2", .slot = "0"}, "<dev-libs/a-2:1"));
}

TEST_CASE("revision ranges compare revisions of the same version") {
    const std::string_view at_least = ">=~dev-libs/a-1.2-r2";
    CHECK(in_range({.cpv = "dev-libs/a-1.2-r2"}, at_least));
    CHECK(in_range({.cpv = "dev-libs/a-1.2-r3"}, at_least));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2-r1"}, at_least));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.3"}, at_least));
    CHECK(in_range({.cpv = "dev-libs/a-1.2-r3"}, ">~dev-libs/a-1.2-r2"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2-r2"}, ">~dev-libs/a-1.2-r2"));
    CHECK(in_range({.cpv = "dev-libs/a-1.2"}, "<~dev-libs/a-1.2-r2"));
    CHECK(in_range({.cpv = "dev-libs/a-1.2-r1"}, "<~dev-libs/a-1.2-r2"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2-r2"}, "<~dev-libs/a-1.2-r2"));
    CHECK(in_range({.cpv = "dev-libs/a-1.2-r2"}, "<=~dev-libs/a-1.2-r2"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.1-r1"}, "<=~dev-libs/a-1.2-r2"));
}

// The glsa module's revisionMatch fails on a slotted revision range (pkgsplit of the atom with
// its slot); egraph honours the slot.
TEST_CASE("a slotted revision range honours its slot") {
    CHECK(in_range({.cpv = "dev-libs/a-1.2-r2", .slot = "1"}, ">=~dev-libs/a-1.2-r1:1"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2-r2", .slot = "0"}, ">=~dev-libs/a-1.2-r1:1"));
}

TEST_CASE("a range that does not parse matches nothing") {
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2"}, ">=~"));
    CHECK_FALSE(in_range({.cpv = "dev-libs/a-1.2"}, "<<dev-libs/a-2"));
}

TEST_CASE("an advisory affects an installed version vulnerable and not unaffected") {
    const auto system = make_system(
        {{.cpv = "dev-libs/a-1.2"}, {.cpv = "dev-libs/b-1.5"}, {.cpv = "dev-libs/c-3"}}, {});
    egraph::test::IndexBuilder index;
    index.advisory("202601-02", "a: overflow",
                   {{"dev-libs/a", "*", "<dev-libs/a-1.4", ">=dev-libs/a-1.4"}}, 3);
    // b's 1.5 is a fixed branch below the fixed mainline.
    index.advisory("202601-01", "b: leak",
                   {{"dev-libs/b", "*", "<dev-libs/b-2", ">=dev-libs/b-2 >=dev-libs/b-1.5"}});
    index.advisory("202601-03", "c: not installed",
                   {{"dev-libs/d", "*", "<dev-libs/d-9", ">=dev-libs/d-9"}});
    const auto found = egraph::affected_advisories(index.index(), system.store, {});
    REQUIRE(ids(found) == std::vector<std::string>{"202601-02"});
    const auto& advisory = found.front();
    CHECK(advisory.title == "a: overflow");
    CHECK(advisory.revision == 3);
    REQUIRE(advisory.packages.size() == 1);
    CHECK(advisory.packages.front().cpv == "dev-libs/a-1.2");
    CHECK(advisory.packages.front().fixed == std::vector<std::string>{">=dev-libs/a-1.4"});
}

TEST_CASE("an advisory's package entries count only on their arches") {
    const auto system = make_system({{.cpv = "dev-libs/a-1"}}, {});
    egraph::test::IndexBuilder index;
    index.advisory("202601-01", "other arch",
                   {{"dev-libs/a", "amd64 arm", "<dev-libs/a-2", ">=dev-libs/a-2"}});
    index.advisory("202601-02", "listed arch",
                   {{"dev-libs/a", "amd64 x86", "<dev-libs/a-2", ">=dev-libs/a-2"}});
    index.advisory("202601-03", "every arch",
                   {{"dev-libs/a", "*", "<dev-libs/a-2", ">=dev-libs/a-2"}});
    CHECK(ids(egraph::affected_advisories(index.index(), system.store, {})) ==
          std::vector<std::string>{"202601-02", "202601-03"});
}

TEST_CASE("applied advisories are left out") {
    const auto system = make_system({{.cpv = "dev-libs/a-1"}}, {});
    egraph::test::IndexBuilder index;
    for (const auto* id : {"202601-01", "202601-02"}) {
        index.advisory(id, "a", {{"dev-libs/a", "*", "<dev-libs/a-2", ">=dev-libs/a-2"}});
    }
    const std::vector<std::string> applied{"202601-01"};
    CHECK(ids(egraph::affected_advisories(index.index(), system.store, applied)) ==
          std::vector<std::string>{"202601-02"});
}

TEST_CASE("every vulnerable slot is listed once, with its entry's fixes") {
    const auto system = make_system(
        {{.cpv = "dev-libs/a-1.2", .slot = "1"}, {.cpv = "dev-libs/a-2.1", .slot = "2"}}, {});
    egraph::test::IndexBuilder index;
    index.advisory("202601-01", "a",
                   {{"dev-libs/a", "*", "<dev-libs/a-1.3:1", ">=dev-libs/a-1.3:1"},
                    {"dev-libs/a", "*", "<dev-libs/a-2.2:2", ">=dev-libs/a-2.2:2"},
                    {"dev-libs/a", "x86", "<dev-libs/a-2.2:2", ">=dev-libs/a-2.2:2"}});
    const auto found = egraph::affected_advisories(index.index(), system.store, {});
    REQUIRE(found.size() == 1);
    const auto& packages = found.front().packages;
    REQUIRE(packages.size() == 2);
    CHECK(packages.at(0).cpv == "dev-libs/a-1.2");
    CHECK(packages.at(0).fixed == std::vector<std::string>{">=dev-libs/a-1.3:1"});
    CHECK(packages.at(1).cpv == "dev-libs/a-2.1");
    CHECK(packages.at(1).fixed == std::vector<std::string>{">=dev-libs/a-2.2:2"});
}

TEST_CASE("applied advisories are read as grabfile reads them") {
    const egraph::test::TempDir root;
    CHECK(egraph::applied_advisories(root.path()).empty());
    std::filesystem::create_directories(root.path() / "var/lib/portage");
    egraph::test::write_text(root.path() / "var/lib/portage/glsa_injected",
                             "# applied\n202601-01\n\n  202601-02  # by hand\n");
    CHECK(egraph::applied_advisories(root.path()) ==
          std::vector<std::string>{"202601-01", "202601-02"});
}
