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

Total: 10 packages (5 upgrades, 4 new, 1 in new slot), Size of downloads: 0 KiB
  (app-misc/host-2:0/0::test_repo, ebuild scheduled for merge) USE="" conflicts with
)x";
    CHECK(
        egraph::parse_pretend(output) ==
        std::vector<PretendMerge>{
            {.cpv = "dev-lang/py-3.14.1", .repo = "test_repo", .kind = "new", .use = ""},
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
        });
}

TEST_CASE("nothing to merge is an empty list") {
    CHECK(egraph::parse_pretend("\nThese are the packages that would be merged, in order:\n\n"
                                "Calculating dependencies ... done!\n\n"
                                "Total: 0 packages, Size of downloads: 0 KiB\n")
              .empty());
    CHECK(egraph::parse_pretend("").empty());
}

TEST_CASE("the plan's merges in emerge's terms, a new package with its USE") {
    auto system = make_system(
        {{.cpv = "app-misc/masked-2"}, {.cpv = "app-misc/up-1"}},
        {{.cpv = "app-misc/up-1"},
         {.cpv = "app-misc/up-2", .deps = {{"RDEPEND", "dev-libs/fresh"}}, .repo = "other"},
         {.cpv = "app-misc/masked-1"},
         {.cpv = "app-misc/masked-2", .visible = false},
         {.cpv = "dev-libs/fresh-1", .iuse = "doc test", .use = "doc"}});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    CHECK(egraph::planned_merges(system.evaluated, plan) ==
          std::vector<PretendMerge>{
              {.cpv = "app-misc/masked-1", .repo = "test_repo", .kind = "downgrade", .use = ""},
              {.cpv = "app-misc/up-2", .repo = "other", .kind = "upgrade", .use = ""},
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
    CHECK(egraph::merge_differences(ours, theirs).empty());
}

TEST_CASE("merge lists differ in cpvs, repositories, kinds and new packages' USE") {
    const std::vector<PretendMerge> ours = {
        {.cpv = "app-misc/only-ours-1", .repo = "r", .kind = "new", .use = ""},
        {.cpv = "app-misc/repo-2", .repo = "overlay", .kind = "upgrade", .use = ""},
        {.cpv = "app-misc/kind-1", .repo = "r", .kind = "rebuild", .use = ""},
        {.cpv = "dev-libs/use-1", .repo = "r", .kind = "new", .use = R"(USE="x -y")"}};
    const std::vector<PretendMerge> theirs = {
        {.cpv = "dev-libs/use-1", .repo = "r", .kind = "new", .use = R"(USE="-x -y")"},
        {.cpv = "app-misc/kind-1", .repo = "r", .kind = "new", .use = ""},
        {.cpv = "app-misc/repo-2", .repo = "r", .kind = "upgrade", .use = ""},
        {.cpv = "app-misc/only-theirs-3", .repo = "r", .kind = "downgrade", .use = ""}};
    CHECK(egraph::merge_differences(ours, theirs) ==
          std::vector<std::string>{
              "app-misc/kind-1::r\tkind\trebuild\tnew",
              "app-misc/only-ours-1::r\tegraph\tnew",
              "app-misc/only-theirs-3::r\temerge\tdowngrade",
              "app-misc/repo-2::overlay\tegraph\tupgrade",
              "app-misc/repo-2::r\temerge\tupgrade",
              "dev-libs/use-1::r\tuse\tUSE=\"x -y\"\tUSE=\"-x -y\"",
          });
}

TEST_CASE("equal versions spelled otherwise are the same merge, as emerge picks among them") {
    const std::vector<PretendMerge> ours = {
        {.cpv = "dev-libs/v-1.0", .repo = "r", .kind = "new", .use = ""}};
    const std::vector<PretendMerge> theirs = {
        {.cpv = "dev-libs/v-1.00", .repo = "r", .kind = "new", .use = ""}};
    CHECK(egraph::merge_differences(ours, theirs).empty());
    const std::vector<PretendMerge> other_repo = {
        {.cpv = "dev-libs/v-1.00", .repo = "overlay", .kind = "new", .use = ""}};
    CHECK(egraph::merge_differences(ours, other_repo).size() == 2);
}
