#include "config_check.hpp"

#include "index_builder.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <tuple>
#include <vector>

using egraph::Finding;
using egraph::FindingKind;

TEST_CASE("findings go by severity, then each file's in line order") {
    std::vector<Finding> findings{
        {.kind = FindingKind::not_installed, .file = "/a", .line = 1, .atom = "x/gone"},
        {.kind = FindingKind::no_effect, .file = "/b", .line = 2, .atom = "x/b", .token = "t"},
        {.kind = FindingKind::contradicted, .file = "/a", .line = 9, .atom = "x/c"},
        {.kind = FindingKind::dead, .file = "/b", .line = 1, .atom = "x/d"},
        {.kind = FindingKind::dead, .file = "/a", .line = 4, .atom = "x/e"},
        {.kind = FindingKind::no_effect, .file = "/b", .line = 2, .atom = "x/b", .token = "s"},
        {.kind = FindingKind::dead, .file = "/a", .line = 2, .atom = "x/f"},
    };
    egraph::order_findings(findings);
    std::vector<std::string> order;
    for (const auto& each : findings) {
        order.push_back(each.atom + each.token);
    }
    CHECK(order == std::vector<std::string>{"x/f", "x/e", "x/d", "x/c", "x/bt", "x/bs", "x/gone"});
}

TEST_CASE("only a dead or contradicted entry fails the check") {
    using egraph::failing;
    const Finding dead{.kind = FindingKind::dead, .file = "/a", .line = 1, .atom = "x/a"};
    const Finding contradicted{.kind = FindingKind::contradicted, .file = "/a", .atom = "x/a"};
    const Finding no_effect{.kind = FindingKind::no_effect, .file = "/a", .atom = "x/a"};
    const Finding not_installed{.kind = FindingKind::not_installed, .file = "/a", .atom = "x/a"};
    CHECK_FALSE(failing(std::vector<Finding>{}));
    CHECK_FALSE(failing(std::vector{no_effect, not_installed}));
    CHECK(failing(std::vector{no_effect, dead}));
    CHECK(failing(std::vector{contradicted}));
}

TEST_CASE("a finding's record names its place, severity, kind, atom, token and message") {
    const Finding finding{.kind = FindingKind::no_effect,
                          .file = "/etc/portage/package.use",
                          .line = 5,
                          .atom = "app-misc/foo",
                          .token = "-bar",
                          .message = "not in the IUSE of anything it matches"};
    CHECK(egraph::finding_record(finding) ==
          "/etc/portage/package.use\t5\twarning\tno-effect\tapp-misc/foo\t-bar\tnot in the IUSE of "
          "anything it matches");
}

TEST_CASE("an entry matching nothing is dead, one matching nothing installed a note") {
    egraph::test::IndexBuilder b;
    std::ignore = b.version({.cpv = "app-misc/a-1"});
    std::ignore = b.version({.cpv = "app-misc/a-2"});
    std::ignore = b.version({.cpv = "app-misc/b-1", .repo = "overlay"});
    std::ignore = b.version({.cpv = "dev-libs/c-1"});
    const auto system =
        egraph::test::make_system({{.cpv = "app-misc/a-1"}, {.cpv = "x11-misc/gone-1"}}, {});
    const std::vector<egraph::UserEntry> entries{
        {.file = "/u", .line = 1, .atom = "app-misc/a"},
        {.file = "/u", .line = 2, .atom = "=app-misc/a-2"},
        {.file = "/u", .line = 3, .atom = "=app-misc/a-3"},
        {.file = "/u", .line = 4, .atom = "app-misc/b"},
        {.file = "/u", .line = 5, .atom = "app-misc/b::gentoo"},
        {.file = "/u", .line = 6, .atom = "dev-libs/*"},
        {.file = "/u", .line = 7, .atom = "app-*/*"},
        {.file = "/u", .line = 8, .atom = "x11-misc/gone"},
        {.file = "/u", .line = 9, .atom = "-dev-libs/nothing"},
        {.file = "/u", .line = 10, .atom = "-app-misc/a"},
        {.file = "/u", .line = 11, .atom = "not an atom"},
    };
    const auto found = egraph::unmatched_entries(entries, system.store, b.index());
    std::vector<std::string> shown;
    for (const auto& each : found) {
        shown.push_back(
            std::format("{} {} {}", each.line, egraph::kind_name(each.kind), each.message));
    }
    // A version of an installed package's cp is enough; a package only installed is matched.
    CHECK(shown == std::vector<std::string>{
                       "3 dead matches nothing in any repository",
                       "4 not-installed matches only packages not installed",
                       "5 dead matches nothing in any repository",
                       "6 not-installed matches only packages not installed",
                       "9 dead matches nothing in any repository",
                       "11 dead is not an atom portage reads",
                   });
}
