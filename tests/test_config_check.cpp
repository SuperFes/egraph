#include "config_check.hpp"

#include "index_builder.hpp"
#include "system_builder.hpp"
#include "use_ledger_builder.hpp"

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

namespace {

// use_findings' records over candidates and a USE ledger, every package.use line in "/pu".
std::vector<std::string> use_records(std::vector<egraph::test::Available> available,
                                     const egraph::test::UseSpec& spec,
                                     const std::vector<egraph::test::OwnLayers>& own = {}) {
    auto system = egraph::test::make_system({}, std::move(available));
    auto& ev = system.evaluated;
    egraph::test::set_ledger(ev, spec);
    egraph::test::Interner intern(ev);
    for (const auto& layers : own) {
        for (auto& candidate : ev.candidates) {
            if (ev.string(candidate.cpv) == layers.cpv) {
                candidate.internal =
                    egraph::test::ids_of(ev, intern, egraph::test::detail::tokens(layers.internal));
            }
        }
    }
    std::vector<std::string> records;
    for (const auto& finding : egraph::use_findings(system.store, ev)) {
        records.push_back(egraph::finding_record(finding));
    }
    return records;
}

} // namespace

TEST_CASE("a flag outside IUSE, already set so, or set back later does nothing") {
    const egraph::test::UseSpec spec{
        .profiles = {{{"use.force",
                       {{.file = "/p/use.force", .line = 3, .tokens = "w"},
                        {.file = "/p/use.force", .line = 4, .tokens = "v"}}}}},
        .package_use = {{.file = "/pu", .line = 1, .atom = "cat/a", .tokens = "nope x -y z"},
                        {.file = "/pu", .line = 2, .atom = "cat/a", .tokens = "y -w"},
                        {.file = "/pu", .line = 3, .atom = "cat/a", .tokens = "z"},
                        {.file = "/pu", .line = 4, .atom = "cat/gone", .tokens = "x"},
                        {.file = "/pu", .line = 5, .atom = "cat/a", .tokens = "-v"}}};
    const auto records = use_records({{.cpv = "cat/a-1", .iuse = "x y z w v"}}, spec,
                                     {{.cpv = "cat/a-1", .internal = "x y w"}});
    CHECK(records ==
          std::vector<std::string>{
              "/pu\t1\twarning\tno-effect\tcat/a\tnope\tnot in the IUSE of anything it matches",
              "/pu\t1\twarning\tno-effect\tcat/a\tx\talready on (its IUSE default)",
              "/pu\t1\terror\tcontradicted\tcat/a\t-y\tline 2 overrides it for everything it "
              "matches",
              "/pu\t1\twarning\tno-effect\tcat/a\tz\tline 3 sets it too",
              "/pu\t2\terror\tcontradicted\tcat/a\t-w\t/p/use.force:3 overrides it for "
              "everything it matches",
              "/pu\t3\twarning\tno-effect\tcat/a\tz\talready on (line 1)",
              // Off already where it stands, but forced on after.
              "/pu\t5\terror\tcontradicted\tcat/a\t-v\t/p/use.force:4 overrides it for "
              "everything it matches",
          });
}

TEST_CASE("a flag that changes anything it matches is no finding, versions apart") {
    const egraph::test::UseSpec spec{
        .package_use = {{.file = "/pu", .line = 1, .atom = "cat/a", .tokens = "x"},
                        {.file = "/pu", .line = 2, .atom = ">=cat/a-2", .tokens = "-x"}}};
    // Line 1 is undone for 2 alone; it still sets x for 1.
    CHECK(use_records({{.cpv = "cat/a-1", .iuse = "x"}, {.cpv = "cat/a-2", .iuse = "x"}}, spec)
              .empty());
}

TEST_CASE("a token after -* in its own line counts on its own") {
    const egraph::test::UseSpec spec{
        .use_expand = {"TARGETS"},
        .profiles = {{{"make.defaults", {{.file = "/md", .var = "TARGETS", .tokens = "x"}}}}},
        .package_use = {
            {.file = "/pu", .line = 1, .atom = "cat/a", .tokens = "-targets_* targets_x"}}};
    CHECK(use_records({{.cpv = "cat/a-1", .iuse = "targets_x targets_y"}}, spec).empty());
}

TEST_CASE("the user profile's package.use counts, other profiles' do not") {
    const egraph::test::UseSpec spec{
        .profiles =
            {{{"package.use", {{.file = "/p/package.use", .atom = "cat/a", .tokens = "no"}}}},
             {{"package.use",
               {{.file = "/etc/portage/profile/package.use", .atom = "cat/a", .tokens = "nope"}}}}},
        .profile_paths = {"/var/db/repos/gentoo/profiles/default", "/etc/portage/profile"}};
    CHECK(use_records({{.cpv = "cat/a-1", .iuse = "x"}}, spec) ==
          std::vector<std::string>{"/etc/portage/profile/package.use\t1\twarning\tno-effect\tcat/"
                                   "a\tnope\tnot in the IUSE of anything it matches"});
}

TEST_CASE("a visibility token or line changing nothing it matches, or a mask undone") {
    egraph::test::IndexBuilder b;
    std::ignore = b.version({.cpv = "app-misc/a-1", .keywords = "~x86"});
    std::ignore = b.version({.cpv = "app-misc/b-1"});
    std::ignore = b.version({.cpv = "app-misc/c-1", .license = "EULA"});
    std::ignore = b.version({.cpv = "app-misc/d-1"});
    std::ignore = b.version({.cpv = "app-misc/gone-1", .keywords = "~x86"});
    auto& ledger = b.ledger();
    ledger.package_mask = b.lines({{.atom = "app-misc/d", .file = "/m", .line = 1}});
    ledger.package_unmask = b.lines({{.atom = "app-misc/d", .file = "/u", .line = 1},
                                     {.atom = "app-misc/b", .file = "/u", .line = 2}});
    ledger.package_accept_keywords =
        b.lines({{.atom = "app-misc/a", .tokens = "~x86 ~arm", .file = "/ak", .line = 1},
                 {.atom = "app-misc/b", .tokens = "~x86", .file = "/ak", .line = 2},
                 {.atom = "=app-misc/b-1", .file = "/ak", .line = 3},
                 {.atom = "app-misc/gone", .file = "/ak", .line = 4}});
    ledger.package_license =
        b.lines({{.atom = "app-misc/c", .tokens = "EULA", .file = "/l", .line = 1}});
    const auto system = egraph::test::make_system({{.cpv = "app-misc/a-1"},
                                                   {.cpv = "app-misc/b-1"},
                                                   {.cpv = "app-misc/c-1"},
                                                   {.cpv = "app-misc/d-1"}},
                                                  {});
    std::vector<std::string> records;
    for (const auto& finding : egraph::visibility_findings(system.store, b.index())) {
        records.push_back(egraph::finding_record(finding));
    }
    // Lines for packages not installed are left to the not-installed notes.
    CHECK(records ==
          std::vector<std::string>{
              "/m\t1\terror\tcontradicted\tapp-misc/d\t\tunmasked again for everything it matches "
              "(/u:1)",
              "/u\t2\twarning\tno-effect\tapp-misc/b\t\tnothing it matches is masked",
              "/ak\t1\twarning\tno-effect\tapp-misc/a\t~arm\tnothing it matches needs it",
              "/ak\t2\twarning\tno-effect\tapp-misc/b\t~x86\tnothing it matches needs it",
              "/ak\t3\twarning\tno-effect\t=app-misc/b-1\t\tnothing it matches needs it",
              "/l\t1\twarning\tno-effect\tapp-misc/c\tEULA\tnothing it matches needs it",
          });
}

TEST_CASE("a mask, unmask or refusal already so names the line that made it so") {
    egraph::test::IndexBuilder b;
    std::ignore = b.version({.cpv = "app-misc/b-1"});
    std::ignore = b.version({.cpv = "app-misc/c-1", .license = "EULA"});
    std::ignore = b.version({.cpv = "app-misc/e-1"});
    std::ignore = b.version({.cpv = "app-misc/f-1"});
    auto& ledger = b.ledger();
    ledger.conf = b.lines({{.var = "ACCEPT_LICENSE", .tokens = "-EULA"}});
    ledger.package_mask = b.lines({{.atom = "app-misc/e", .file = "/m", .line = 1},
                                   {.atom = ">=app-misc/e-1", .file = "/m", .line = 2},
                                   {.atom = "app-misc/f", .file = "/m", .line = 3}});
    ledger.package_unmask = b.lines({{.atom = "app-misc/f", .file = "/u", .line = 1},
                                     {.atom = "=app-misc/f-1", .file = "/u", .line = 2}});
    ledger.package_license =
        b.lines({{.atom = "app-misc/c", .tokens = "-EULA", .file = "/l", .line = 1},
                 {.atom = "app-misc/b", .tokens = "-EULA", .file = "/l", .line = 2}});
    const auto system = egraph::test::make_system({{.cpv = "app-misc/b-1"},
                                                   {.cpv = "app-misc/c-1"},
                                                   {.cpv = "app-misc/e-1"},
                                                   {.cpv = "app-misc/f-1"}},
                                                  {});
    std::vector<std::string> records;
    for (const auto& finding : egraph::visibility_findings(system.store, b.index())) {
        records.push_back(egraph::finding_record(finding));
    }
    CHECK(records ==
          std::vector<std::string>{
              "/m\t1\twarning\tno-effect\tapp-misc/e\t\talready masked (line 2)",
              "/m\t2\twarning\tno-effect\t>=app-misc/e-1\t\talready masked (line 1)",
              "/m\t3\terror\tcontradicted\tapp-misc/f\t\tunmasked again for everything it matches "
              "(/u:1)",
              "/u\t1\twarning\tno-effect\tapp-misc/f\t\talready unmasked (line 2)",
              "/u\t2\twarning\tno-effect\t=app-misc/f-1\t\talready unmasked (line 1)",
              "/l\t1\twarning\tno-effect\tapp-misc/c\t-EULA\talready refused",
              "/l\t2\twarning\tno-effect\tapp-misc/b\t-EULA\tchanges nothing for anything it "
              "matches",
          });
}
