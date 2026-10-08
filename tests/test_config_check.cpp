#include "config_check.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
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
