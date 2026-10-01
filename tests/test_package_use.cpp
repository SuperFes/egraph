#include "cli.hpp"
#include "helpers.hpp"
#include "package_use.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using egraph::test::make_system;
using egraph::test::TempDir;

namespace {

std::string contents(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("USE changes go to package.use, or zz-autounmask in its directory") {
    const TempDir dir;
    std::filesystem::create_directories(dir.path() / "etc/portage");
    CHECK(egraph::package_use_path(dir.path()) == dir.path() / "etc/portage/package.use");
    std::filesystem::create_directories(dir.path() / "etc/portage/package.use");
    CHECK(egraph::package_use_path(dir.path()) ==
          dir.path() / "etc/portage/package.use/zz-autounmask");
}

TEST_CASE("appending starts on a line of its own, creating the file") {
    const TempDir dir;
    const auto path = dir.path() / "package.use";
    CHECK_FALSE(egraph::append_to_file(path, "a/b x\n"));
    CHECK(contents(path) == "a/b x\n");
    std::ofstream(path, std::ios::app) << "c/d y";
    CHECK_FALSE(egraph::append_to_file(path, "e/f z\n"));
    CHECK(contents(path) == "a/b x\nc/d y\ne/f z\n");
    CHECK(egraph::append_to_file(dir.path() / "missing/package.use", "x\n").has_value());
}

TEST_CASE("the plan's USE changes read as emerge writes them for package.use") {
    const auto system = make_system(
        {}, {{.cpv = "dev-libs/lib-1", .iuse = "gtk qt", .use = "qt"},
             {.cpv = "app-misc/want-1", .deps = {{"RDEPEND", "dev-libs/lib[gtk,-qt]"}}}});
    egraph::Targets targets{.scope = {}, .roots = true, .deep = false};
    targets.request.push_back({.set = "", .atom = "app-misc/want"});
    targets.selection = egraph::Selection::reinstall;
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none, targets);
    CHECK(egraph::package_use_text(system.store, system.evaluated, plan) ==
          "# required by app-misc/want-1::test_repo\n"
          "# required by app-misc/want (argument)\n"
          ">=dev-libs/lib-1 gtk -qt\n");
}

TEST_CASE("only y or yes is yes") {
    for (const auto* answer : {"y\n", "yes\n", " Y \n"}) {
        std::istringstream in(answer);
        std::ostringstream out;
        CHECK(egraph::answered_yes(in, out, "Write?"));
        CHECK(out.str() == "Write? [y/N] ");
    }
    for (const auto* answer : {"\n", "n\n", "yess\n", ""}) {
        std::istringstream in(answer);
        std::ostringstream out;
        CHECK_FALSE(egraph::answered_yes(in, out, "Write?"));
    }
}
