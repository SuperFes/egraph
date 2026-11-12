#include "action.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::execution_options;
using egraph::Readiness;
using egraph::Stop;
using egraph::stop_before_verifying;

namespace {

std::vector<std::string> passed(std::vector<std::string> defaults) {
    return execution_options(defaults);
}

} // namespace

TEST_CASE("only options that change how emerge runs pass from EMERGE_DEFAULT_OPTS") {
    CHECK(passed({}).empty());
    CHECK(
        passed({"--ask", "--verbose", "--usepkg", "--newuse", "--deep", "--with-bdeps=y"}).empty());
    CHECK(passed({"--jobs=4", "--load-average=5.5", "--keep-going", "--quiet-build=y"}) ==
          std::vector<std::string>{"--jobs=4", "--load-average=5.5", "--keep-going",
                                   "--quiet-build=y"});
    CHECK(passed({"--quiet-fail", "--fail-clean=n", "--buildpkg", "--nospinner", "--alert"}) ==
          std::vector<std::string>{"--quiet-fail", "--fail-clean=n", "--buildpkg", "--nospinner",
                                   "--alert"});
}

TEST_CASE("an option's value may follow it as the next word") {
    CHECK(passed({"--jobs", "4", "--load-average", "3"}) ==
          std::vector<std::string>{"--jobs=4", "--load-average=3"});
    CHECK(passed({"--keep-going", "y", "--quiet-build", "n"}) ==
          std::vector<std::string>{"--keep-going=y", "--quiet-build=n"});
    // A word that is not a value is the next option's.
    CHECK(passed({"--jobs", "--keep-going", "--ask", "y"}) ==
          std::vector<std::string>{"--jobs", "--keep-going"});
    CHECK(passed({"--buildpkg-exclude", "dev-lang/rust", "--ask"}) ==
          std::vector<std::string>{"--buildpkg-exclude=dev-lang/rust"});
    // A dropped option's value goes with it.
    CHECK(passed({"--backtrack", "30", "--jobs=2"}) == std::vector<std::string>{"--jobs=2"});
    CHECK(passed({"--exclude", "sys-libs/glibc", "--keep-going"}) ==
          std::vector<std::string>{"--keep-going"});
}

TEST_CASE("short options are split, -j taking its count") {
    CHECK(passed({"-av"}).empty());
    CHECK(passed({"-j4"}) == std::vector<std::string>{"--jobs=4"});
    CHECK(passed({"-avj3"}) == std::vector<std::string>{"--jobs=3"});
    CHECK(passed({"-j", "8", "-b"}) == std::vector<std::string>{"--jobs=8", "--buildpkg"});
    CHECK(passed({"-j", "-a"}) == std::vector<std::string>{"--jobs"});
    CHECK(passed({"-q"}) == std::vector<std::string>{"--quiet"});
}

TEST_CASE("the jobs emerge runs at once come from the last --jobs") {
    using Words = std::vector<std::string>;
    CHECK(egraph::jobs_of(Words{}) == 1);
    CHECK(egraph::jobs_of(Words{"--keep-going"}) == 1);
    CHECK(egraph::jobs_of(Words{"--jobs=4"}) == 4);
    CHECK(egraph::jobs_of(Words{"--jobs=4", "--jobs=2"}) == 2);
    CHECK(egraph::jobs_of(Words{"--jobs"}) == std::nullopt);
    CHECK(egraph::jobs_of(Words{"--jobs", "--jobs=3"}) == 3);
    CHECK(egraph::jobs_of(egraph::execution_options(Words{"-aj8"})) == 8);
}

TEST_CASE("exec's jobs: -j, else EMERGE_DEFAULT_OPTS, else any under a jobserver, else one") {
    using Words = std::vector<std::string>;
    CHECK(egraph::exec_jobs(3, Words{"--jobs=4"}, true) == 3);
    CHECK(egraph::exec_jobs(std::nullopt, Words{"--jobs=4"}, true) == 4);
    CHECK(egraph::exec_jobs(std::nullopt, Words{"--jobs"}, false) == std::nullopt);
    CHECK(egraph::exec_jobs(std::nullopt, Words{"--keep-going"}, true) == std::nullopt);
    CHECK(egraph::exec_jobs(std::nullopt, Words{}, false) == 1);
}

TEST_CASE("emerge goes on after a failure by the last --keep-going") {
    using Words = std::vector<std::string>;
    CHECK_FALSE(egraph::keep_going_of(Words{}));
    CHECK(egraph::keep_going_of(Words{"--keep-going"}));
    CHECK(egraph::keep_going_of(Words{"--keep-going=y"}));
    CHECK_FALSE(egraph::keep_going_of(Words{"--keep-going=y", "--keep-going=n"}));
    CHECK(egraph::keep_going_of(Words{"--keep-going=n", "--keep-going"}));
    CHECK(egraph::keep_going_of(egraph::execution_options(Words{"--keep-going", "y"})));
}

TEST_CASE("the free space a second job needs comes from the last --jobs-tmpdir-require-free-gb") {
    using Words = std::vector<std::string>;
    CHECK(egraph::tmpdir_free_gb_of(Words{}) == 18);
    CHECK(egraph::tmpdir_free_gb_of(Words{"--jobs-tmpdir-require-free-gb=0"}) == 0);
    CHECK(egraph::tmpdir_free_gb_of(
              Words{"--jobs-tmpdir-require-free-gb=4", "--jobs-tmpdir-require-free-gb=9"}) == 9);
    // Passed on to emerge, its value joined.
    CHECK(egraph::execution_options(Words{"--jobs-tmpdir-require-free-gb", "2"}) ==
          Words{"--jobs-tmpdir-require-free-gb=2"});
}

TEST_CASE("emerge carries out the request as verified, asking nothing") {
    const egraph::EmergeRequest request{.targets = {"@world"},
                                        .update = true,
                                        .deep = true,
                                        .noreplace = false,
                                        .rebuilds = egraph::UseRebuilds::all,
                                        .dynamic_deps = false};
    const std::vector<std::string> jobs{"--jobs=4"};
    CHECK(egraph::run_arguments(request, true, jobs) ==
          std::vector<std::string>{"--ignore-default-opts", "--ask=n", "--jobs=4", "--update",
                                   "--deep", "--newuse", "--dynamic-deps=n", "--oneshot",
                                   "@world"});
    const egraph::EmergeRequest install{.targets = {"app-misc/a", "@set"},
                                        .update = false,
                                        .deep = false,
                                        .noreplace = true,
                                        .rebuilds = egraph::UseRebuilds::none,
                                        .dynamic_deps = true};
    CHECK(egraph::run_arguments(install, false, {}) ==
          std::vector<std::string>{"--ignore-default-opts", "--ask=n", "--noreplace", "app-misc/a",
                                   "@set"});
}

TEST_CASE("emerge's depclean uninstalls the replaced slots, asking nothing") {
    egraph::EmergeRequest request{.targets = {"@world"},
                                  .update = true,
                                  .deep = true,
                                  .noreplace = false,
                                  .rebuilds = egraph::UseRebuilds::all,
                                  .dynamic_deps = true};
    const std::vector<std::string> cpvs{"app-misc/wine-1", "sys-kernel/sources-2"};
    const std::vector<std::string> jobs{"--jobs=4"};
    CHECK(egraph::depclean_arguments(request, cpvs, jobs) ==
          std::vector<std::string>{"--ignore-default-opts", "--ask=n", "--jobs=4", "--depclean",
                                   "=app-misc/wine-1", "=sys-kernel/sources-2"});
    request.dynamic_deps = false;
    CHECK(egraph::depclean_arguments(request, cpvs, {}) ==
          std::vector<std::string>{"--ignore-default-opts", "--ask=n", "--dynamic-deps=n",
                                   "--depclean", "=app-misc/wine-1", "=sys-kernel/sources-2"});
}

TEST_CASE(
    "an action stops before verifying for refusals, nothing, privileges, a preview, then a yes") {
    const Readiness ready{
        .refused = false, .empty = false, .writable = true, .yes = false, .can_ask = true};
    CHECK_FALSE(stop_before_verifying(ready));
    auto readiness = ready;
    readiness.can_ask = false;
    CHECK(stop_before_verifying(readiness) == Stop::unconfirmed);
    readiness.yes = true;
    CHECK_FALSE(stop_before_verifying(readiness));
    readiness.preview = true;
    CHECK(stop_before_verifying(readiness) == Stop::previewed);
    readiness.writable = false;
    CHECK(stop_before_verifying(readiness) == Stop::unprivileged);
    readiness.empty = true;
    CHECK(stop_before_verifying(readiness) == Stop::nothing);
    readiness.refused = true;
    CHECK(stop_before_verifying(readiness) == Stop::refused);
}

TEST_CASE("a plan emerge lists otherwise says whose list a yes carries out") {
    CHECK(egraph::differing_note("exec", "merge", egraph::Carrier::egraph) ==
          "egraph: exec: emerge would merge otherwise; on yes egraph merges its plan as shown");
    CHECK(egraph::differing_note("update", "merge", egraph::Carrier::emerge) ==
          "egraph: update: emerge would merge otherwise; on yes emerge merges its own list");
    CHECK(egraph::differing_note("remove", "remove", egraph::Carrier::emerge) ==
          "egraph: remove: emerge would remove otherwise; on yes emerge removes its own list");
}

TEST_CASE("a plan emerge lists otherwise is asked for anyway") {
    CHECK(egraph::anyway("Have emerge merge this plan?") == "Have emerge merge this plan anyway?");
    CHECK(egraph::anyway("Merge") == "Merge anyway?");
}

TEST_CASE("a || alternative pins its package names") {
    CHECK(egraph::alternative_cps("dev-lang/zig-bin:0.17") ==
          std::vector<std::string>{"dev-lang/zig-bin"});
    CHECK(egraph::alternative_cps("( >=dev-lang/a-1[x] !dev-lang/b dev-lang/c:2 dev-lang/a )") ==
          std::vector<std::string>{"dev-lang/a", "dev-lang/c"});
}

TEST_CASE("an answer picks an alternative by its number, the first by default") {
    CHECK(egraph::chosen_alternative("", 2) == 0);
    CHECK(egraph::chosen_alternative(" 2 ", 2) == 1);
    CHECK(egraph::chosen_alternative("1", 2) == 0);
    CHECK(!egraph::chosen_alternative("3", 2));
    CHECK(!egraph::chosen_alternative("0", 2));
    CHECK(!egraph::chosen_alternative("b", 2));
    CHECK(!egraph::chosen_alternative("1x", 2));
}

TEST_CASE("what emerge runs under is read from egraph-build's JSON") {
    const auto settings = egraph::parse_run_settings(R"({
 "options": ["--jobs", "4"],
 "elog": {"summary": "/var/log/portage/elog/summary.log", "system": "save_summary"},
 "jobserver": "/run/jobserver", "merge_wait": false, "portage_installed": true,
 "tmpdir": "/var/tmp", "distdir": "/var/cache/distfiles", "ccache_dir": "/var/cache/ccache",
 "repositories": ["/var/db/repos/gentoo", "/var/db/repos/local"]
})");
    REQUIRE(settings);
    CHECK(settings->defaults == std::vector<std::string>{"--jobs", "4"});
    CHECK(settings->jobserver == "/run/jobserver");
    CHECK(settings->tmpdir == "/var/tmp");
    CHECK(settings->distdir == "/var/cache/distfiles");
    CHECK(settings->ccache_dir == "/var/cache/ccache");
    CHECK(settings->repositories ==
          std::vector<std::string>{"/var/db/repos/gentoo", "/var/db/repos/local"});
    CHECK_FALSE(settings->merge_wait);
    CHECK(settings->portage_installed);
    CHECK(settings->elog_summary == "/var/log/portage/elog/summary.log");
    CHECK(settings->elog_system == "save_summary");
    const auto plain =
        egraph::parse_run_settings(R"({"options": [], "elog": {"summary": null, "system": null},
                                       "jobserver": null, "merge_wait": true,
                                       "portage_installed": false, "tmpdir": "/var/tmp",
                                       "distdir": "/d", "ccache_dir": null,
                                       "repositories": []})");
    REQUIRE(plain);
    CHECK_FALSE(plain->jobserver);
    CHECK(plain->merge_wait);
    CHECK_FALSE(plain->portage_installed);
    CHECK_FALSE(plain->elog_summary);
    CHECK_FALSE(plain->elog_system);
    CHECK_FALSE(plain->ccache_dir);
    CHECK(plain->repositories.empty());
    CHECK_FALSE(egraph::parse_run_settings(
        R"({"options": [], "elog": {"summary": null, "system": null}, "jobserver": null,
            "merge_wait": true, "portage_installed": false, "tmpdir": "/var/tmp",
            "distdir": "/d", "ccache_dir": null, "repositories": [1]})"));
    CHECK_FALSE(egraph::parse_run_settings(
        R"({"options": [], "elog": {"summary": null, "system": null}, "jobserver": null,
            "merge_wait": true, "portage_installed": false, "tmpdir": "/var/tmp",
            "ccache_dir": null, "repositories": []})"));
    CHECK_FALSE(egraph::parse_run_settings("--jobs\n4\n"));
    CHECK_FALSE(egraph::parse_run_settings(R"({"options": [1], "elog": {}})"));
    CHECK_FALSE(egraph::parse_run_settings(R"({"options": [], "elog": {"summary": 1}})"));
    CHECK_FALSE(egraph::parse_run_settings(R"({"options": []})"));
    CHECK_FALSE(egraph::parse_run_settings(
        R"({"options": [], "elog": {"summary": null, "system": null}, "jobserver": 3,
            "merge_wait": true, "tmpdir": "/var/tmp"})"));
    CHECK_FALSE(egraph::parse_run_settings(
        R"({"options": [], "elog": {"summary": null, "system": null}, "jobserver": null,
            "merge_wait": "yes", "portage_installed": false, "tmpdir": "/var/tmp"})"));
    CHECK_FALSE(egraph::parse_run_settings(
        R"({"options": [], "elog": {"summary": null, "system": null}, "jobserver": null,
            "merge_wait": true, "tmpdir": "/var/tmp"})"));
    CHECK_FALSE(egraph::parse_run_settings(
        R"({"options": [], "elog": {"summary": null, "system": null}, "jobserver": null})"));
}

TEST_CASE("the merge-wait scope is the last --merge-wait-scope, deep by default") {
    CHECK(egraph::merge_wait_scope_of({}) == "deep");
    const std::vector<std::string> passed{"--merge-wait-scope=none", "--jobs=2",
                                          "--merge-wait-scope=toolchain"};
    CHECK(egraph::merge_wait_scope_of(passed) == "toolchain");
    const std::vector<std::string> defaults{"--merge-wait-scope", "system", "--ask"};
    CHECK(egraph::execution_options(defaults) ==
          std::vector<std::string>{"--merge-wait-scope=system"});
}
