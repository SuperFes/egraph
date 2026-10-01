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

TEST_CASE("an action stops before verifying for refusals, nothing, privileges, then a yes") {
    const Readiness ready{
        .refused = false, .empty = false, .writable = true, .yes = false, .can_ask = true};
    CHECK_FALSE(stop_before_verifying(ready));
    auto readiness = ready;
    readiness.can_ask = false;
    CHECK(stop_before_verifying(readiness) == Stop::unconfirmed);
    readiness.yes = true;
    CHECK_FALSE(stop_before_verifying(readiness));
    readiness.writable = false;
    CHECK(stop_before_verifying(readiness) == Stop::unprivileged);
    readiness.empty = true;
    CHECK(stop_before_verifying(readiness) == Stop::nothing);
    readiness.refused = true;
    CHECK(stop_before_verifying(readiness) == Stop::refused);
}
