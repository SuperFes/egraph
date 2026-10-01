#include "verify.hpp"

#include "plan.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::PretendMerge;
using egraph::test::make_system;

namespace {

std::vector<std::string> arguments(const egraph::EmergeRequest& request) {
    return egraph::pretend_arguments(request);
}

// The request's own options and arguments after those pretend_arguments always puts first.
std::vector<std::string> with_fixed(const std::vector<std::string>& rest) {
    std::vector<std::string> all = {"--pretend", "--verbose", "--color=n", "--nospinner",
                                    "--ignore-default-opts"};
    all.insert(all.end(), rest.begin(), rest.end());
    return all;
}

} // namespace

TEST_CASE("emerge is asked for the same request, pretending, verbose and uncoloured") {
    CHECK(arguments({.targets = {"@world"}}) == with_fixed({"@world"}));
    CHECK(arguments({.targets = {"app-misc/a", "=dev-libs/b-1"}, .noreplace = true}) ==
          with_fixed({"--noreplace", "app-misc/a", "=dev-libs/b-1"}));
    CHECK(arguments({.targets = {"@installed"},
                     .update = true,
                     .deep = true,
                     .rebuilds = egraph::UseRebuilds::all}) ==
          with_fixed({"--update", "--deep", "--newuse", "@installed"}));
    CHECK(arguments({.targets = {"@world"},
                     .update = true,
                     .rebuilds = egraph::UseRebuilds::changed,
                     .dynamic_deps = false}) ==
          with_fixed({"--update", "--changed-use", "--dynamic-deps=n", "@world"}));
}

TEST_CASE("emerge's merge list is read from its verbose lines") {
    const auto output = R"x(
These are the packages that would be merged, in order:

Calculating dependencies ... done!
Dependency resolution took 0.03 s (backtrack: 4/20).

[ebuild  NS    ] dev-lang/py-3.14.1:3.14::test_repo [3.13.1:3.13::test_repo] 0 KiB
[ebuild  N     ] dev-libs/chain-1::test_repo  0 KiB
[ebuild  N     ] dev-libs/fresh-1::test_repo  USE="a9 a10 (fixed) on -off (-stuck)" PYTHON_TARGETS="(py3_12) py3_13 -py3_11" 0 KiB
[ebuild     U  ] app-misc/flagged-2::test_repo [1::test_repo] USE="extra%*" 0 KiB
[ebuild     UD ] app-misc/down-1:0/1::other [2:0/2::test_repo] 12,345 KiB
[ebuild   R    ] app-misc/same-1::test_repo  USE="doc* -test" 0 KiB
[ebuild  rR    ] app-misc/bound-1::test_repo  0 KiB
[ebuild  N    ~] app-misc/testing-1::test_repo  0 KiB
[binary  N     ] app-misc/built-1-2::test_repo  0 KiB
[ebuild  N F   ] app-misc/fetch-1::test_repo to /mnt/gentoo/ USE="x" 0 KiB
[blocks b      ] app-misc/old ("app-misc/old" is soft blocking app-misc/new-1)
[uninstall     ] app-misc/gone-1::test_repo 
[uninstall     ] dev-libs/s-1:1::test_repo 
[blocks B      ] app-misc/kept ("app-misc/kept" is hard blocking app-misc/b-1, app-misc/a-1)
[blocks B      ] <dev-libs/lib-2[x] (is soft blocking app-misc/app-2)

Total: 10 packages (5 upgrades, 4 new, 1 in new slot), Size of downloads: 0 KiB
  (app-misc/host-2:0/0::test_repo, ebuild scheduled for merge) USE="" conflicts with
)x";
    const auto found = egraph::parse_pretend(output, false);
    CHECK(found.blocks == std::vector<egraph::PretendBlock>{
                              {.atom = "<dev-libs/lib-2[x]", .holder = "app-misc/app-2"},
                              {.atom = "app-misc/kept", .holder = "app-misc/a-1"},
                              {.atom = "app-misc/kept", .holder = "app-misc/b-1"}});
    CHECK(
        found.merges ==
        std::vector<PretendMerge>{
            {.cpv = "dev-lang/py-3.14.1", .repo = "test_repo", .kind = "new-slot", .use = ""},
            {.cpv = "dev-libs/chain-1", .repo = "test_repo", .kind = "new", .use = ""},
            {.cpv = "dev-libs/fresh-1",
             .repo = "test_repo",
             .kind = "new",
             .use =
                 R"x(USE="a9 a10 (fixed) on -off (-stuck)" PYTHON_TARGETS="(py3_12) py3_13 -py3_11")x"},
            {.cpv = "app-misc/flagged-2",
             .repo = "test_repo",
             .kind = "upgrade",
             .use = R"x(USE="extra%*")x"},
            {.cpv = "app-misc/down-1", .repo = "other", .kind = "downgrade", .use = ""},
            {.cpv = "app-misc/same-1",
             .repo = "test_repo",
             .kind = "rebuild",
             .use = R"x(USE="doc* -test")x"},
            {.cpv = "app-misc/bound-1", .repo = "test_repo", .kind = "rebuild", .use = ""},
            {.cpv = "app-misc/testing-1", .repo = "test_repo", .kind = "new", .use = ""},
            {.cpv = "app-misc/built-1-2", .repo = "test_repo", .kind = "new", .use = ""},
            {.cpv = "app-misc/fetch-1", .repo = "test_repo", .kind = "new", .use = R"x(USE="x")x"},
            {.cpv = "app-misc/gone-1", .repo = "test_repo", .kind = "uninstall", .use = ""},
            {.cpv = "dev-libs/s-1", .repo = "test_repo", .kind = "uninstall", .use = ""},
        });
}

TEST_CASE("nothing to merge is an empty list") {
    CHECK(egraph::parse_pretend("\nThese are the packages that would be merged, in order:\n\n"
                                "Calculating dependencies ... done!\n\n"
                                "Total: 0 packages, Size of downloads: 0 KiB\n",
                                false) == egraph::Pretend{});
    CHECK(egraph::parse_pretend("", false) == egraph::Pretend{});
}

TEST_CASE("the plan's merges in emerge's terms, a new package with its USE") {
    auto system = make_system({{.cpv = "app-misc/masked-2"},
                               {.cpv = "app-misc/up-1"},
                               {.cpv = "dev-lang/py-1", .slot = "1"}},
                              {{.cpv = "app-misc/up-1"},
                               {.cpv = "app-misc/up-2",
                                .deps = {{"RDEPEND", "dev-libs/fresh dev-lang/py:2"}},
                                .repo = "other"},
                               {.cpv = "dev-lang/py-1", .slot = "1"},
                               {.cpv = "dev-lang/py-2", .slot = "2"},
                               {.cpv = "app-misc/masked-1"},
                               {.cpv = "app-misc/masked-2", .visible = false},
                               {.cpv = "dev-libs/fresh-1", .iuse = "doc test", .use = "doc"}});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    CHECK(egraph::planned_merges(system.store, system.evaluated, plan).merges ==
          std::vector<PretendMerge>{
              {.cpv = "app-misc/masked-1", .repo = "test_repo", .kind = "downgrade", .use = ""},
              {.cpv = "app-misc/up-2", .repo = "other", .kind = "upgrade", .use = ""},
              {.cpv = "dev-lang/py-2", .repo = "test_repo", .kind = "new-slot", .use = ""},
              {.cpv = "dev-libs/fresh-1",
               .repo = "test_repo",
               .kind = "new",
               .use = R"(USE="doc -test")"},
          });
}

TEST_CASE("the same merge lists in any order do not differ") {
    const std::vector<PretendMerge> ours = {
        {.cpv = "app-misc/a-2", .repo = "r", .kind = "upgrade", .use = ""},
        {.cpv = "dev-libs/b-1", .repo = "r", .kind = "new", .use = R"(USE="x")"}};
    const std::vector<PretendMerge> theirs = {
        {.cpv = "dev-libs/b-1", .repo = "r", .kind = "new", .use = R"(USE="x")"},
        // emerge shows a replacement's USE, which egraph does not compare.
        {.cpv = "app-misc/a-2", .repo = "r", .kind = "upgrade", .use = R"(USE="doc*")"}};
    CHECK(egraph::merge_differences(
              {.merges = ours, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}},
              {.merges = theirs, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}})
              .empty());
}

TEST_CASE("merge lists differ in cpvs, repositories, kinds and new packages' USE") {
    const std::vector<PretendMerge> ours = {
        {.cpv = "app-misc/only-ours-1", .repo = "r", .kind = "new", .use = ""},
        {.cpv = "app-misc/repo-2", .repo = "overlay", .kind = "upgrade", .use = ""},
        {.cpv = "app-misc/kind-1", .repo = "r", .kind = "rebuild", .use = ""},
        {.cpv = "dev-libs/use-1", .repo = "r", .kind = "new", .use = R"(USE="x -y")"},
        {.cpv = "dev-libs/slot-2", .repo = "r", .kind = "new-slot", .use = R"(USE="x")"},
        {.cpv = "dev-libs/apart-2", .repo = "r", .kind = "new", .use = ""}};
    const std::vector<PretendMerge> theirs = {
        {.cpv = "dev-libs/use-1", .repo = "r", .kind = "new", .use = R"(USE="-x -y")"},
        {.cpv = "dev-libs/slot-2", .repo = "r", .kind = "new-slot", .use = R"(USE="-x")"},
        {.cpv = "dev-libs/apart-2", .repo = "r", .kind = "new-slot", .use = ""},
        {.cpv = "app-misc/kind-1", .repo = "r", .kind = "new", .use = ""},
        {.cpv = "app-misc/repo-2", .repo = "r", .kind = "upgrade", .use = ""},
        {.cpv = "app-misc/only-theirs-3", .repo = "r", .kind = "downgrade", .use = ""}};
    CHECK(
        egraph::merge_differences(
            {.merges = ours, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}},
            {.merges = theirs, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}}) ==
        std::vector<std::string>{
            "app-misc/kind-1::r\tkind\trebuild\tnew",
            "app-misc/only-ours-1::r\tegraph\tnew",
            "app-misc/only-theirs-3::r\temerge\tdowngrade",
            "app-misc/repo-2::overlay\tegraph\tupgrade",
            "app-misc/repo-2::r\temerge\tupgrade",
            "dev-libs/apart-2::r\tkind\tnew\tnew-slot",
            "dev-libs/slot-2::r\tuse\tUSE=\"x\"\tUSE=\"-x\"",
            "dev-libs/use-1::r\tuse\tUSE=\"x -y\"\tUSE=\"-x -y\"",
        });
}

TEST_CASE("equal versions spelled otherwise are the same merge, as emerge picks among them") {
    const std::vector<PretendMerge> ours = {
        {.cpv = "dev-libs/v-1.0", .repo = "r", .kind = "new", .use = ""}};
    const std::vector<PretendMerge> theirs = {
        {.cpv = "dev-libs/v-1.00", .repo = "r", .kind = "new", .use = ""}};
    CHECK(egraph::merge_differences(
              {.merges = ours, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}},
              {.merges = theirs, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}})
              .empty());
    const std::vector<PretendMerge> other_repo = {
        {.cpv = "dev-libs/v-1.00", .repo = "overlay", .kind = "new", .use = ""}};
    CHECK(
        egraph::merge_differences(
            {.merges = ours, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}},
            {.merges = other_repo, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}})
            .size() == 2);
}

TEST_CASE("the plan's uninstalls and blocks in emerge's terms") {
    const auto system =
        make_system({{.cpv = "app-misc/old-1"},
                     {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/old"}}}},
                    {{.cpv = "app-misc/new-1", .deps = {{"RDEPEND", "!!app-misc/old"}}},
                     {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", "app-misc/old"}}},
                     {.cpv = "app-misc/user-2", .deps = {{"RDEPEND", "app-misc/new"}}}},
                    {"app-misc/user"});
    egraph::Targets world{.scope = {}, .roots = true, .deep = false};
    auto found = egraph::planned_merges(
        system.store, system.evaluated,
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none, world));
    CHECK(found.blocks ==
          std::vector<egraph::PretendBlock>{{.atom = "app-misc/old", .holder = "app-misc/new-1"}});
    world.running_root = false;
    found = egraph::planned_merges(
        system.store, system.evaluated,
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none, world));
    CHECK(found.blocks.empty());
    CHECK(
        found.merges.back() ==
        PretendMerge{.cpv = "app-misc/old-1", .repo = "test_repo", .kind = "uninstall", .use = ""});
}

TEST_CASE("blockers only one side cannot resolve differ") {
    const egraph::Pretend ours{
        .merges = {},
        .blocks = {{.atom = "a/x", .holder = "a/both-1"}, {.atom = "a/y", .holder = "a/ours-1"}},
        .unsatisfied = {},
        .unmet = {},
        .use_changes = {}};
    const egraph::Pretend theirs{
        .merges = {},
        .blocks = {{.atom = "a/x", .holder = "a/both-1"}, {.atom = "a/z", .holder = "a/theirs-1"}},
        .unsatisfied = {},
        .unmet = {},
        .use_changes = {}};
    CHECK(
        egraph::merge_differences(ours, theirs) ==
        std::vector<std::string>{"a/ours-1\tegraph\tblocks a/y", "a/theirs-1\temerge\tblocks a/z"});
}

TEST_CASE("emerge's refusal names what nothing satisfies") {
    const auto found = egraph::parse_pretend(R"(
emerge: there are no ebuilds to satisfy "dev-libs/missing".
(dependency required by "app-misc/a-1::test_repo" [ebuild])
!!! All ebuilds that could satisfy "dev-libs/testing" have been masked.
!!! One of the following masked packages is required to complete your request:
)",
                                             true);
    CHECK(found.merges.empty());
    CHECK(found.unsatisfied == std::vector<std::string>{"dev-libs/missing", "dev-libs/testing"});
}

TEST_CASE("an update emerge skips for what nothing satisfies is no refusal") {
    const auto found = egraph::parse_pretend(R"(
These are the packages that would be merged, in order:

[ebuild  N     ] dev-libs/pulled-1::test_repo  0 KiB

!!! The following update has been skipped due to unsatisfied dependencies:

emerge: there are no ebuilds to satisfy "dev-libs/missing".
(dependency required by "dev-libs/pulled-2::test_repo" [ebuild])
)",
                                             false);
    CHECK(found.merges.size() == 1);
    CHECK(found.unsatisfied.empty());
}

TEST_CASE("a refusal differs only for what emerge names and ours lacks, or ours alone") {
    const std::vector<egraph::PretendMerge> merges{
        {.cpv = "a/x-1", .repo = "gentoo", .kind = "new", .use = ""}};
    const egraph::Pretend ours{.merges = merges,
                               .blocks = {},
                               .unsatisfied = {"dev-libs/missing", "dev-libs/other"},
                               .unmet = {},
                               .use_changes = {}};
    // emerge names one of them and prints no merge list.
    CHECK(egraph::merge_differences(ours, {.merges = {},
                                           .blocks = {},
                                           .unsatisfied = {"dev-libs/missing"},
                                           .unmet = {},
                                           .use_changes = {}})
              .empty());
    CHECK(egraph::merge_differences(ours, {.merges = {},
                                           .blocks = {},
                                           .unsatisfied = {"dev-libs/gone"},
                                           .unmet = {},
                                           .use_changes = {}}) ==
          std::vector<std::string>{"dev-libs/gone\temerge\tunsatisfied"});
    CHECK(
        egraph::merge_differences(
            ours,
            {.merges = merges, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}}) ==
        std::vector<std::string>{"dev-libs/missing\tegraph\tunsatisfied",
                                 "dev-libs/other\tegraph\tunsatisfied"});
}

TEST_CASE("emerge's unmet REQUIRED_USE names the version it selected") {
    const auto found = egraph::parse_pretend(R"(
These are the packages that would be merged, in order:

!!! Problem resolving dependencies for app-misc/req
!!! The ebuild selected to satisfy "app-misc/req" has unmet requirements.
- app-misc/req-1::test_repo USE="-a -b"

  The following REQUIRED_USE flag constraints are unsatisfied:
    exactly-one-of ( a b )

)",
                                             true);
    CHECK(found.merges.empty());
    CHECK(found.unmet == std::vector<std::string>{"app-misc/req-1::test_repo"});
}

TEST_CASE("unmet REQUIRED_USE differs only for what emerge names and ours lacks, or ours alone") {
    const std::vector<egraph::PretendMerge> merges{
        {.cpv = "a/req-1", .repo = "gentoo", .kind = "new", .use = ""}};
    const egraph::Pretend ours{.merges = merges,
                               .blocks = {},
                               .unsatisfied = {},
                               .unmet = {"a/req-1::gentoo"},
                               .use_changes = {}};
    CHECK(egraph::merge_differences(ours, {.merges = {},
                                           .blocks = {},
                                           .unsatisfied = {},
                                           .unmet = {"a/req-1::gentoo"},
                                           .use_changes = {}})
              .empty());
    // emerge stops at the first refusal it finds, whichever kind.
    CHECK(egraph::merge_differences(ours, {.merges = {},
                                           .blocks = {},
                                           .unsatisfied = {"dev-libs/missing"},
                                           .unmet = {"a/other-1::gentoo"},
                                           .use_changes = {}}) ==
          std::vector<std::string>{"a/other-1::gentoo\temerge\trequired-use",
                                   "dev-libs/missing\temerge\tunsatisfied"});
    CHECK(
        egraph::merge_differences(
            ours,
            {.merges = merges, .blocks = {}, .unsatisfied = {}, .unmet = {}, .use_changes = {}}) ==
        std::vector<std::string>{"a/req-1::gentoo\tegraph\trequired-use"});
}

TEST_CASE("emerge's USE changes are read as package and flags, flags by name") {
    const auto found = egraph::parse_pretend(R"(
These are the packages that would be merged, in order:

[ebuild  N     ] dev-libs/lib-2::test_repo  USE="gtk -qt" 0 KiB

The following USE changes are necessary to proceed:
 (see "package.use" in the portage(5) man page for more details)
# required by app-misc/two-1::test_repo
# required by app-misc/two (argument)
>=dev-libs/lib-2 gtk -qt
# required by app-misc/two-1::test_repo
=dev-libs/pinned-1:0 x
>=dev-libs/other-1:2 -b a

!!! Unrelated
)",
                                             true);
    CHECK(found.merges.size() == 1);
    CHECK(found.use_changes == std::vector<std::string>{"dev-libs/lib-2 gtk -qt",
                                                        "dev-libs/other-1 a -b",
                                                        "dev-libs/pinned-1 x"});
}

TEST_CASE("USE changes differ only as changes, emerge's merge list made before them") {
    const std::vector<egraph::PretendMerge> merges{
        {.cpv = "dev-libs/gtkdep-1", .repo = "gentoo", .kind = "new", .use = ""}};
    const egraph::Pretend ours{.merges = merges,
                               .blocks = {},
                               .unsatisfied = {},
                               .unmet = {},
                               .use_changes = {"dev-libs/lib-2 gtk"}};
    CHECK(egraph::merge_differences(ours, {.merges = {},
                                           .blocks = {},
                                           .unsatisfied = {},
                                           .unmet = {},
                                           .use_changes = {"dev-libs/lib-2 gtk"}})
              .empty());
    CHECK(egraph::merge_differences(ours, {.merges = {},
                                           .blocks = {},
                                           .unsatisfied = {},
                                           .unmet = {},
                                           .use_changes = {"dev-libs/lib-2 -qt gtk"}}) ==
          std::vector<std::string>{"dev-libs/lib-2\tegraph\tuse-change gtk",
                                   "dev-libs/lib-2\temerge\tuse-change -qt gtk"});
}
