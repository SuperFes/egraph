#include "helpers.hpp"
#include "system_builder.hpp"
#include "use_ledger_builder.hpp"
#include "use_stack.hpp"
#include "what_if.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using egraph::WhatIfLine;
using egraph::test::Available;
using Line = egraph::test::UseLine;
using Spec = egraph::test::UseSpec;
using Strings = std::vector<std::string>;

namespace {

constexpr std::string_view config = "/c";

egraph::test::System system_with(std::vector<Available> available, const Spec& spec) {
    auto system = egraph::test::make_system({{.cpv = "cat/dep-1"}}, std::move(available));
    egraph::test::set_ledger(system.evaluated, spec);
    return system;
}

WhatIfLine use(std::string atom, Strings tokens) {
    return {.file = WhatIfLine::File::use, .atom = std::move(atom), .tokens = std::move(tokens)};
}

WhatIfLine env(std::string atom, Strings tokens) {
    return {.file = WhatIfLine::File::env, .atom = std::move(atom), .tokens = std::move(tokens)};
}

const egraph::Candidate& candidate(const egraph::Evaluated& evaluated, std::string_view cpv) {
    for (const auto& c : evaluated.candidates) {
        if (evaluated.string(c.cpv) == cpv) {
            return c;
        }
    }
    FAIL("no candidate " << cpv);
    return evaluated.candidates.front();
}

Strings use_of(const egraph::Evaluated& evaluated, std::string_view cpv) {
    Strings found;
    for (const auto id : evaluated.ids_in(candidate(evaluated, cpv).use)) {
        found.emplace_back(evaluated.string(id));
    }
    return found;
}

egraph::Evaluated tried(const egraph::test::System& system, const std::vector<WhatIfLine>& lines) {
    auto result = egraph::with_what_if(system.evaluated, system.store, lines, config);
    REQUIRE(result.has_value());
    return std::move(*result);
}

// a-1 and b-1 with IUSE x y, nothing enabled; a pulls in cat/dep with x.
std::vector<Available> two() {
    return {{.cpv = "cat/a-1", .deps = {{"RDEPEND", "x? ( cat/dep )"}}, .iuse = "x y"},
            {.cpv = "cat/b-1", .iuse = "x y"}};
}

} // namespace

TEST_CASE("a what-if line is an atom and its tokens, or flags alone for every package") {
    using File = WhatIfLine::File;
    const auto line = egraph::parse_what_if(File::use, "  app-misc/a x  -y ");
    REQUIRE(line.has_value());
    CHECK(line->file == File::use);
    CHECK(line->atom == "app-misc/a");
    CHECK(line->tokens == Strings{"x", "-y"});

    const auto global = egraph::parse_what_if(File::use, "VIDEO_CARDS: radeon -x");
    REQUIRE(global.has_value());
    CHECK(global->atom == "*/*");
    CHECK(global->tokens == Strings{"VIDEO_CARDS:", "radeon", "-x"});
    CHECK(egraph::parse_what_if(File::use, "*/* x")->atom == "*/*");
    CHECK(egraph::parse_what_if(File::use, ">=app-misc/a-2:1::gentoo x")->atom ==
          ">=app-misc/a-2:1::gentoo");

    const auto envs = egraph::parse_what_if(File::env, "app-misc/a clang.conf lto.conf");
    REQUIRE(envs.has_value());
    CHECK(envs->file == File::env);
    CHECK(envs->tokens == Strings{"clang.conf", "lto.conf"});
    CHECK(egraph::parse_what_if(File::env, "*/* clang.conf")->atom == "*/*");

    CHECK_FALSE(egraph::parse_what_if(File::use, "").has_value());
    CHECK_FALSE(egraph::parse_what_if(File::use, "app-misc/a").has_value());
    CHECK_FALSE(egraph::parse_what_if(File::use, "app-misc/a[x] y").has_value());
    CHECK_FALSE(egraph::parse_what_if(File::use, "not/an/atom x").has_value());
    // An env file is always for an atom: */* names every package.
    CHECK_FALSE(egraph::parse_what_if(File::env, "clang.conf").has_value());
    CHECK_FALSE(egraph::parse_what_if(File::env, "app-misc/a").has_value());
}

TEST_CASE("what-if lines are saved to egraph's own files") {
    using File = WhatIfLine::File;
    CHECK(egraph::what_if_path("/nowhere/etc/portage", File::use) ==
          "/nowhere/etc/portage/package.use/egraph");
    CHECK(egraph::what_if_path("/nowhere/etc/portage", File::env) ==
          "/nowhere/etc/portage/package.env/egraph");
}

TEST_CASE("a package's flag tried changes its USE and the dependencies it reduces to") {
    const auto system = system_with(two(), {});
    const auto rdepend = egraph::dep_kinds.size() - 1;
    REQUIRE(use_of(system.evaluated, "cat/a-1").empty());
    REQUIRE(system.evaluated.candidates.front().deps.at(rdepend).count == 0);

    const auto evaluated = tried(system, {use("cat/a", {"x"})});
    CHECK(use_of(evaluated, "cat/a-1") == Strings{"x"});
    CHECK(use_of(evaluated, "cat/b-1").empty());
    const auto deps = evaluated.nodes_in(candidate(evaluated, "cat/a-1").deps.at(rdepend));
    REQUIRE(deps.size() == 1);
    CHECK(evaluated.string(deps.front().atom) == "cat/dep");
    CHECK(evaluated.ids_in(deps.front().matches).size() == 1);

    // Its stack names egraph's file, on the first line of a file not yet there.
    const egraph::UseStacker stacker(system.store, evaluated);
    const auto stacked = stacker.stack(candidate(evaluated, "cat/a-1"));
    const auto& step = stacked.steps.at("x").back();
    REQUIRE(step.entry.has_value());
    const auto& entry = evaluated.ledger_entries.at(*step.entry);
    CHECK(evaluated.string(entry.file) == "/c/package.use/egraph");
    CHECK(entry.line == 1);
    CHECK(evaluated.string(entry.atom) == "cat/a");
}

TEST_CASE("no line, or one changing nothing, leaves the store as it was") {
    const auto system = system_with(two(), {});
    const auto none = tried(system, {});
    CHECK(none.ledger_entries.size() == system.evaluated.ledger_entries.size());
    // y is off already: the entry is there, the candidates are not rebuilt.
    const auto off = tried(system, {use("cat/a", {"-y"})});
    CHECK(off.ledger_entries.size() == system.evaluated.ledger_entries.size() + 1);
    CHECK(off.nodes.size() == system.evaluated.nodes.size());
}

TEST_CASE("a tried line is read where egraph's file sorts among the user's") {
    const auto before = Line{.file = "/c/package.use/00-base", .atom = "cat/a", .tokens = "-x"};
    const auto after = Line{.file = "/c/package.use/zz-last", .atom = "cat/a", .tokens = "-x"};
    // After 00-base, so it wins over it; before zz-last, which wins over it.
    CHECK(use_of(tried(system_with(two(), {.package_use = {before}}), {use("cat/a", {"x"})}),
                 "cat/a-1") == Strings{"x"});
    CHECK(use_of(tried(system_with(two(), {.package_use = {after}}), {use("cat/a", {"x"})}),
                 "cat/a-1")
              .empty());
    // Within a subdirectory sorting before it, too.
    const auto nested = Line{.file = "/c/package.use/dir/zz", .atom = "cat/a", .tokens = "-x"};
    CHECK(use_of(tried(system_with(two(), {.package_use = {nested}}), {use("cat/a", {"x"})}),
                 "cat/a-1") == Strings{"x"});
    // egraph's file already there: after its lines, numbered on from its last.
    const auto saved =
        Line{.file = "/c/package.use/egraph", .line = 4, .atom = "cat/a", .tokens = "-x"};
    const auto evaluated =
        tried(system_with(two(), {.package_use = {saved}}), {use("cat/a", {"x"})});
    CHECK(use_of(evaluated, "cat/a-1") == Strings{"x"});
    CHECK(evaluated.ledger_entries.at(evaluated.ledger.package_use.first + 1).line == 5);
}

TEST_CASE("a more specific atom of the user's still wins over a tried cp") {
    const auto exact = Line{.file = "/c/package.use/00", .atom = "=cat/a-1", .tokens = "-x"};
    CHECK(use_of(tried(system_with(two(), {.package_use = {exact}}), {use("cat/a", {"x"})}),
                 "cat/a-1")
              .empty());
}

TEST_CASE("flags tried for every package join make.conf's, under the packages' own lines") {
    const auto make_conf = Line{.file = "/c/make.conf", .tokens = "-x"};
    const auto global = use("*/*", {"x"});
    const auto both = tried(system_with(two(), {.conf = {make_conf}}), {global});
    CHECK(use_of(both, "cat/a-1") == Strings{"x"});
    CHECK(use_of(both, "cat/b-1") == Strings{"x"});
    // A */* line of a file sorting after egraph's still comes after.
    const auto later = Line{.file = "/c/package.use/zz", .atom = "*/*", .tokens = "-x"};
    CHECK(use_of(tried(system_with(two(), {.conf = {make_conf, later}}), {global}), "cat/a-1")
              .empty());
    // A package's line is a later layer.
    const auto own = Line{.file = "/c/package.use/00", .atom = "cat/b", .tokens = "-x"};
    const auto evaluated = tried(system_with(two(), {.package_use = {own}}), {global});
    CHECK(use_of(evaluated, "cat/a-1") == Strings{"x"});
    CHECK(use_of(evaluated, "cat/b-1").empty());
}

TEST_CASE("USE_EXPAND prefixes in a tried line name the expanded flags") {
    const auto system =
        system_with({{.cpv = "cat/a-1", .iuse = "video_cards_radeon video_cards_intel"}}, {});
    CHECK(use_of(tried(system, {use("cat/a", {"VIDEO_CARDS:", "radeon", "-intel"})}), "cat/a-1") ==
          Strings{"video_cards_radeon"});
}

TEST_CASE("an env file tried for a package brings its USE, the ledger's ranges kept apart") {
    const Spec spec{
        .package_use = {{.file = "/c/package.use/00", .atom = "cat/b", .tokens = "x"}},
        .package_env = {{.file = "/c/package.env", .atom = "cat/b", .tokens = "keep.conf"}},
        .env_files = {{"clang.conf", {{.file = "/c/env/clang.conf", .tokens = "x"}}},
                      {"keep.conf", {{.file = "/c/env/keep.conf", .tokens = "y"}}}}};
    auto available = two();
    available.back().use = "x y";
    const auto system = system_with(std::move(available), spec);
    // A package.use line shifts the entries after it; b's own lines still read the same.
    const auto evaluated = tried(system, {use("cat/a", {"y"}), env("cat/a", {"clang.conf"})});
    CHECK(use_of(evaluated, "cat/a-1") == Strings{"x", "y"});
    CHECK(use_of(evaluated, "cat/b-1") == Strings{"x", "y"});
    const egraph::UseStacker stacker(system.store, evaluated);
    CHECK(stacker.stack(candidate(evaluated, "cat/b-1")).use == Strings{"x", "y"});
    // For every package, its USE is make.conf's.
    const auto everywhere = tried(system, {env("*/*", {"clang.conf"})});
    CHECK(use_of(everywhere, "cat/a-1") == Strings{"x"});

    const auto unknown = egraph::with_what_if(system.evaluated, system.store,
                                              std::vector{env("cat/a", {"gone.conf"})}, config);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error() == "env/gone.conf: no such env file");
}

TEST_CASE("wildcards tried reach every flag they name, in whatever IUSE has them") {
    const Spec spec{.conf = {{.file = "/c/make.conf", .tokens = "y video_cards_vesa"}}};
    const auto system = system_with({{.cpv = "cat/a-1", .iuse = "x y", .use = "y"},
                                     {.cpv = "cat/v-1",
                                      .iuse = "video_cards_vesa video_cards_intel",
                                      .use = "video_cards_vesa"}},
                                    spec);
    const auto cleared = tried(system, {use("*/*", {"-*"})});
    CHECK(use_of(cleared, "cat/a-1").empty());
    CHECK(use_of(cleared, "cat/v-1").empty());
    const auto cards = tried(system, {use("*/*", {"VIDEO_CARDS:", "-*", "intel"})});
    CHECK(use_of(cards, "cat/a-1") == Strings{"y"});
    CHECK(use_of(cards, "cat/v-1") == Strings{"video_cards_intel"});
}

TEST_CASE("what a tried flag pulls in that was never evaluated is named, for evaluating") {
    auto system = system_with({{.cpv = "cat/a-1",
                                .deps = {{"RDEPEND", "x? ( cat/new cat/dep cat/gone !cat/old )"}},
                                .iuse = "x"},
                               {.cpv = "cat/dep-1"}},
                              {});
    egraph::test::add_repository_cps(system, {"cat/a", "cat/dep", "cat/new", "cat/old"});
    CHECK(egraph::newly_reached(system.evaluated, tried(system, {use("cat/a", {"-x"})})).empty());
    // cat/gone has no ebuild, cat/old is only blocked.
    CHECK(egraph::newly_reached(system.evaluated, tried(system, {use("cat/a", {"x"})})) ==
          Strings{"cat/new"});
}

TEST_CASE("a tried flag rebuilds an installed version as --newuse would, from its own ebuild too") {
    const auto at = [](const egraph::Evaluated& evaluated, egraph::Range range) {
        Strings found;
        for (const auto id : evaluated.ids_in(range)) {
            found.emplace_back(evaluated.string(id));
        }
        return found;
    };
    // Its own version is the best: tried, it becomes the target, rebuilt for the flag.
    auto same = egraph::test::make_system({{.cpv = "cat/a-1", .iuse = "x y", .use = "y"}},
                                          {{.cpv = "cat/a-1", .iuse = "x y", .use = "y"}});
    egraph::test::set_ledger(same.evaluated, {.conf = {{.file = "/c/make.conf", .tokens = "y"}}});
    REQUIRE_FALSE(same.evaluated.packages.front().target.has_value());
    const auto on = tried(same, {use("cat/a", {"x", "-y"})});
    REQUIRE(on.packages.front().target == 0U);
    CHECK(at(on, on.packages.front().rebuild) == Strings{"x*", "-y*"});
    CHECK(at(on, on.packages.front().own_rebuild) == Strings{"x*", "-y*"});
    // Set back as it was built, it is no target again.
    const auto back = tried(same, {use("cat/a", {"x", "-y"}), use("cat/a", {"-x", "y"})});
    CHECK_FALSE(back.packages.front().target.has_value());
    CHECK(at(back, back.packages.front().rebuild).empty());

    // A newer version stays the target; its own version's flags are what -uU falls back to.
    auto newer = egraph::test::make_system(
        {{.cpv = "cat/a-1", .iuse = "x"}},
        {{.cpv = "cat/a-1", .iuse = "x"}, {.cpv = "cat/a-2", .iuse = "x"}});
    egraph::test::set_ledger(newer.evaluated, {});
    const auto tried_newer = tried(newer, {use("cat/a", {"x"})});
    const auto& pkg = tried_newer.packages.front();
    REQUIRE(pkg.target == 1U);
    CHECK(at(tried_newer, pkg.rebuild).empty());
    REQUIRE(pkg.own == 0U);
    CHECK(at(tried_newer, pkg.own_rebuild) == Strings{"x*"});
}

TEST_CASE("what lines tried change in a plan is each merge added, dropped or changed") {
    const std::vector<std::string> before{
        "cat/up-1\tupgrade\tcat/up-2\tgentoo",
        "cat/gone-1\tupgrade\tcat/gone-2\tgentoo",
        "cat/flags-1\trebuild\tcat/flags-1\tgentoo\tx*",
        "cat/new-1\tnew\tcat/new-1\tgentoo\tUSE=\"a\"\tcat/up-2 cat/new",
        "cat/held-1\theld\tcat/held-2\tgentoo\t\tcat/x-1 <cat/held-2",
        "cat/x-1\tmasked\tgentoo\tpackage.mask"};
    const std::vector<std::string> after{
        "cat/up-1\tupgrade\tcat/up-2\tgentoo",
        "cat/flags-1\trebuild\tcat/flags-1\tgentoo\tx* -y*",
        "cat/gcc-1\trebuild\tcat/gcc-1\tgentoo\t-nls*",
        "cat/new-1\tnew\tcat/new-1\tgentoo\tUSE=\"a b\"\tcat/up-2 cat/new",
        "cat/server-1\tnew\tcat/server-1\tgentoo\t\tcat/gcc-1 cat/server",
        "cat/held-1\theld\tcat/held-2\tgentoo\t\tcat/x-1 <cat/held-2"};
    CHECK(egraph::tried_lines(before, after) ==
          Strings{"cat/flags-1\ttried\tchanged\trebuild\tcat/flags-1\tgentoo\tx* -y*",
                  "cat/gcc-1\ttried\tadded\trebuild\tcat/gcc-1\tgentoo\t-nls*",
                  "cat/new-1\ttried\tchanged\tnew\tcat/new-1\tgentoo\tUSE=\"a b\"",
                  "cat/server-1\ttried\tadded\tnew\tcat/server-1\tgentoo\t",
                  "cat/gone-1\ttried\tdropped\tupgrade\tcat/gone-2\tgentoo\t"});
    CHECK(egraph::tried_lines(before, before).empty());
}

TEST_CASE("an env file tried changes how the installed packages it matches build") {
    const Spec spec{
        .package_env = {{.file = "/c/package.env", .atom = "cat/b", .tokens = "keep.conf"}},
        .env_files = {{"clang.conf", {{.file = "/c/env/clang.conf", .tokens = "x"}}},
                      {"keep.conf", {{.file = "/c/env/keep.conf", .tokens = "y"}}}}};
    auto system = egraph::test::make_system({{.cpv = "cat/a-1"}, {.cpv = "cat/b-1"}},
                                            {{.cpv = "cat/a-1"}, {.cpv = "cat/b-1"}});
    egraph::test::set_ledger(system.evaluated, spec);
    const auto changes_of = [&](const std::vector<WhatIfLine>& lines) {
        return egraph::env_lines(system.store, egraph::env_changes(system.store, system.evaluated,
                                                                   tried(system, lines), lines));
    };
    CHECK(changes_of({use("cat/a", {"x"})}).empty());
    CHECK(changes_of({env("cat/a", {"clang.conf"})}) == Strings{"cat/a-1\tenv\t\tclang.conf"});
    // b had keep.conf already.
    CHECK(changes_of({env("cat/b", {"keep.conf"})}) ==
          Strings{"cat/b-1\tenv\tkeep.conf\tkeep.conf keep.conf"});
    CHECK(changes_of({env("*/*", {"clang.conf"})}) ==
          Strings{"cat/a-1\tenv\t\tclang.conf", "cat/b-1\tenv\tkeep.conf\tkeep.conf clang.conf"});
}

namespace {

WhatIfLine use_line(std::string atom, Strings tokens) {
    return {.file = WhatIfLine::File::use, .atom = std::move(atom), .tokens = std::move(tokens)};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("saving to egraph's file starts it, one line per atom") {
    const std::vector<WhatIfLine> lines{use_line("media-video/mpv", {"-xv"}),
                                        use_line("*/*", {"-nls"}),
                                        use_line("media-video/mpv", {"vaapi"})};
    CHECK(egraph::saved_text("", lines, true) ==
          "# Lines egraph tried and saved; egraph keeps one line per atom here.\n"
          "media-video/mpv -xv vaapi\n"
          "*/* -nls\n");
}

TEST_CASE("saving merges per atom: a flag's token replaces its old one, an emptied line goes") {
    const std::string existing = "# kept\n"
                                 "media-video/mpv -xv vaapi\n"
                                 "\n"
                                 "app-misc/a x\n"
                                 "dev-lang/python PYTHON_TARGETS: python3_13 -python3_12\n";
    const std::vector<WhatIfLine> lines{use_line("media-video/mpv", {"xv", "-lua"}),
                                        use_line("app-misc/a", {"-x"}),
                                        use_line("dev-lang/python", {"-python_targets_python3_13"}),
                                        use_line("app-misc/new", {"y"})};
    CHECK(egraph::saved_text(existing, lines, true) ==
          "# kept\n"
          "media-video/mpv vaapi xv -lua\n"
          "\n"
          "app-misc/a -x\n"
          "dev-lang/python -python_targets_python3_12 -python_targets_python3_13\n"
          "app-misc/new y\n");
    // A token taking back the only one leaves nothing: the line goes.
    CHECK(egraph::saved_text("app-misc/a x\n", std::vector{use_line("app-misc/a", {})}, true) ==
          "app-misc/a x\n");
}

TEST_CASE("saving env lines merges their files, and the user's own file is only appended to") {
    const std::vector<WhatIfLine> env{{.file = WhatIfLine::File::env,
                                       .atom = "sys-devel/gcc",
                                       .tokens = {"lto.conf", "clang.conf"}}};
    CHECK(egraph::saved_text("sys-devel/gcc clang.conf big.conf\n", env, true) ==
          "sys-devel/gcc big.conf lto.conf clang.conf\n");
    const std::vector<WhatIfLine> use{use_line("app-misc/a", {"PYTHON_TARGETS:", "python3_14"})};
    CHECK(egraph::saved_text("app-misc/a x", use, false) ==
          "app-misc/a x\napp-misc/a PYTHON_TARGETS: python3_14\n");
    CHECK(egraph::saved_text("", use, false) == "app-misc/a PYTHON_TARGETS: python3_14\n");
}

TEST_CASE("the files saved: egraph's beside the user's, or the user's single file, written") {
    const egraph::test::TempDir directory;
    const auto& root = directory.path();
    std::filesystem::create_directories(root / "package.use");
    egraph::test::write_text(root / "package.use" / "egraph", "app-misc/a x\n");
    egraph::test::write_text(root / "package.env", "sys-devel/gcc big.conf\n");
    const std::vector<WhatIfLine> lines{
        use_line("app-misc/a", {"-x"}),
        {.file = WhatIfLine::File::env, .atom = "app-misc/a", .tokens = {"clang.conf"}}};
    const auto files = egraph::saved_files(root, lines);
    REQUIRE(files.has_value());
    REQUIRE(files->size() == 2);
    CHECK(files->front().path == root / "package.use" / "egraph");
    CHECK(files->front().text == "app-misc/a -x\n");
    CHECK(files->back().path == root / "package.env");
    CHECK(files->back().text == "sys-devel/gcc big.conf\napp-misc/a clang.conf\n");
    std::filesystem::permissions(root / "package.env", std::filesystem::perms::owner_read |
                                                           std::filesystem::perms::owner_write);
    REQUIRE(egraph::write_saved(*files).has_value());
    CHECK(read_text(root / "package.use" / "egraph") == "app-misc/a -x\n");
    CHECK(read_text(root / "package.env") == "sys-devel/gcc big.conf\napp-misc/a clang.conf\n");
    CHECK(std::filesystem::status(root / "package.env").permissions() ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    CHECK(std::ranges::distance(std::filesystem::directory_iterator(root)) == 2);

    // Without package.use at all, its directory is made.
    const egraph::test::TempDir bare;
    const auto made = egraph::saved_files(bare.path(), std::vector{use_line("*/*", {"-nls"})});
    REQUIRE(made.has_value());
    REQUIRE(egraph::write_saved(*made).has_value());
    CHECK(std::filesystem::is_directory(bare.path() / "package.use"));
    CHECK(read_text(bare.path() / "package.use" / "egraph").ends_with("*/* -nls\n"));

    // A file that cannot be written says so.
    const egraph::SavedFile blocked{.path = root / "package.use" / "egraph" / "below", .text = ""};
    const auto failed = egraph::write_saved(std::vector{blocked});
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().contains("package.use/egraph/below"));
}
