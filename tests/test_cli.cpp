#include "cli.hpp"
#include "tui.hpp"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <variant>

namespace {

egraph::Invocation parse(const std::string& line) {
    CLI::App app;
    egraph::Invocation invocation;
    egraph::configure(app, invocation);
    app.parse(line, false);
    return invocation;
}

} // namespace

TEST_CASE("a package argument lands in its command") {
    const auto invocation = parse("rdeps dev-libs/openssl");
    const auto* rdeps = std::get_if<egraph::Rdeps>(&invocation.command);
    REQUIRE(rdeps != nullptr);
    CHECK(rdeps->packages == std::vector<std::string>{"dev-libs/openssl"});
    CHECK_FALSE(invocation.store.has_value());
    CHECK_FALSE(invocation.no_refresh);
    CHECK(invocation.root == std::filesystem::path{"/"});
    CHECK_FALSE(invocation.config_root.has_value());
    CHECK_FALSE(invocation.eprefix.has_value());
    CHECK_FALSE(invocation.builder.has_value());
}

TEST_CASE("emerge pretends under the same roots") {
    const egraph::EmergeRequest request{.targets = {"@world"}, .update = true};
    const auto with = [&request](std::vector<std::string> head) {
        const auto rest = egraph::pretend_arguments(request);
        head.insert(head.end(), rest.begin(), rest.end());
        return head;
    };
    egraph::Invocation invocation;
    CHECK(egraph::emerge_command(invocation, request) == with({"emerge", "--root", "/"}));
    invocation.emerge = "/opt/emerge";
    invocation.root = "/mnt/gentoo";
    invocation.config_root = "/mnt/config";
    invocation.eprefix = "/prefix";
    CHECK(egraph::emerge_command(invocation, request) ==
          with({"/opt/emerge", "--root", "/mnt/gentoo", "--config-root", "/mnt/config", "--prefix",
                "/prefix"}));
}

TEST_CASE("roots are passed through") {
    const auto invocation = parse("--root /mnt/target --config-root /mnt/config broken");
    CHECK(invocation.root == std::filesystem::path{"/mnt/target"});
    CHECK(invocation.config_root == std::filesystem::path{"/mnt/config"});
}

TEST_CASE("global options precede the command") {
    const auto invocation = parse("--store /tmp/x.egraph --no-refresh broken");
    CHECK(std::holds_alternative<egraph::Broken>(invocation.command));
    CHECK(invocation.store == std::filesystem::path{"/tmp/x.egraph"});
    CHECK(invocation.no_refresh);
}

TEST_CASE("deps and rdeps take several packages") {
    const auto invocation = parse("deps app-misc/a dev-libs/b-1");
    const auto* deps = std::get_if<egraph::Deps>(&invocation.command);
    REQUIRE(deps != nullptr);
    CHECK(deps->packages == std::vector<std::string>{"app-misc/a", "dev-libs/b-1"});
}

TEST_CASE("soname lists consumers unless asked for providers") {
    const auto consumers = parse("soname libz.so.1");
    CHECK_FALSE(std::get<egraph::Soname>(consumers.command).providers);
    const auto providers = parse("soname --providers libz.so.1");
    CHECK(std::get<egraph::Soname>(providers.command).providers);
    CHECK(std::get<egraph::Soname>(providers.command).soname == "libz.so.1");
}

TEST_CASE("export neighborhoods take a depth and a direction") {
    const auto plain = std::get<egraph::Export>(parse("export a/b").command);
    CHECK(plain.depth == 1);
    CHECK(plain.direction == egraph::Direction::reverse);
    const auto both =
        std::get<egraph::Export>(parse("export --depth 3 --direction both a/b").command);
    CHECK(both.depth == 3);
    CHECK(both.direction == egraph::Direction::both);
    CHECK_THROWS_AS(parse("export --direction sideways a/b"), CLI::ValidationError);
}

TEST_CASE("export takes a format and any number of packages") {
    const auto plain = parse("export");
    const auto* defaults = std::get_if<egraph::Export>(&plain.command);
    REQUIRE(defaults != nullptr);
    CHECK(defaults->format == egraph::ExportFormat::dot);
    CHECK(defaults->packages.empty());

    const auto json = parse("export --format JSON app-misc/a app-misc/b");
    const auto* exported = std::get_if<egraph::Export>(&json.command);
    REQUIRE(exported != nullptr);
    CHECK(exported->format == egraph::ExportFormat::json);
    CHECK(exported->packages == std::vector<std::string>{"app-misc/a", "app-misc/b"});
}

TEST_CASE("orphans takes emerge's --with-bdeps") {
    CHECK(std::get<egraph::Orphans>(parse("orphans").command).build_deps);
    CHECK_FALSE(std::get<egraph::Orphans>(parse("orphans --with-bdeps n").command).build_deps);
    CHECK(std::get<egraph::Orphans>(parse("orphans --with-bdeps y").command).build_deps);
    CHECK_THROWS_AS(parse("orphans --with-bdeps maybe"), CLI::ValidationError);
}

TEST_CASE("updates takes emerge's --newuse and --changed-use") {
    using egraph::UseRebuilds;
    const auto rebuilds = [](const std::string& command) {
        return std::get<egraph::Updates>(parse(command).command).rebuilds;
    };
    CHECK(rebuilds("updates") == UseRebuilds::none);
    CHECK(rebuilds("updates --newuse") == UseRebuilds::all);
    CHECK(rebuilds("updates -N") == UseRebuilds::all);
    CHECK(rebuilds("updates --changed-use") == UseRebuilds::changed);
    CHECK(rebuilds("updates -U") == UseRebuilds::changed);
    CHECK(rebuilds("updates -U -N") == UseRebuilds::all);
    CHECK(rebuilds("updates -N -U") == UseRebuilds::all);
    CHECK_FALSE(parse("updates --dynamic-deps n").dynamic_deps);
    CHECK(std::get<egraph::Updates>(parse("updates --held").command).held);
}

TEST_CASE("updates is plain emerge -u unless -D asks for --deep") {
    const auto deep = [](const std::string& command) {
        return std::get<egraph::Updates>(parse(command).command).deep;
    };
    CHECK_FALSE(deep("updates"));
    CHECK_FALSE(deep("updates --world"));
    CHECK(deep("updates -D"));
    CHECK(deep("updates --deep --world"));
    CHECK(deep("updates -DN"));
}

TEST_CASE("plan takes emerge's targets and its selection options") {
    const auto plan = [](const std::string& command) {
        return std::get<egraph::PlanCommand>(parse(command).command);
    };
    const auto plain = plan("plan app-misc/foo @world");
    CHECK(plain.targets == std::vector<std::string>{"app-misc/foo", "@world"});
    CHECK_FALSE(plain.update);
    CHECK_FALSE(plain.noreplace);
    const auto deep = plan("plan -uDN @world");
    CHECK(deep.update);
    CHECK(deep.deep);
    CHECK(deep.rebuilds == egraph::UseRebuilds::all);
    CHECK(plan("plan -n foo").noreplace);
    CHECK(plan("plan -uU foo").rebuilds == egraph::UseRebuilds::changed);
    CHECK_FALSE(plain.verify);
    CHECK(plan("plan --verify foo").verify);
    CHECK_THROWS(parse("plan"));
}

TEST_CASE("updates and plan can be verified against emerge, the one in PATH by default") {
    CHECK_FALSE(std::get<egraph::Updates>(parse("updates").command).verify);
    CHECK(std::get<egraph::Updates>(parse("updates --verify -D").command).verify);
    CHECK_FALSE(parse("updates").emerge.has_value());
    CHECK(parse("--emerge /opt/emerge updates --verify").emerge == "/opt/emerge");
    CHECK_THROWS(parse("orphans --verify"));
}

TEST_CASE("update takes updates' options and --yes, and always verifies") {
    const auto update = std::get<egraph::Update>(parse("update -DN --world --yes").command);
    CHECK(update.deep);
    CHECK(update.world);
    CHECK(update.rebuilds == egraph::UseRebuilds::all);
    CHECK(update.yes);
    CHECK_FALSE(std::get<egraph::Update>(parse("update -y").command).world);
    CHECK_FALSE(std::get<egraph::Update>(parse("update").command).yes);
    CHECK_FALSE(parse("update --dynamic-deps n").dynamic_deps);
    CHECK_THROWS(parse("update --verify"));
}

TEST_CASE("install takes plan's options, --oneshot and --yes") {
    const auto install = std::get<egraph::Install>(parse("install -1y -uD a/b @set").command);
    CHECK(install.targets == std::vector<std::string>{"a/b", "@set"});
    CHECK(install.update);
    CHECK(install.deep);
    CHECK(install.oneshot);
    CHECK(install.yes);
    const auto plain = std::get<egraph::Install>(parse("install -n a/b").command);
    CHECK(plain.noreplace);
    CHECK_FALSE(plain.oneshot);
    CHECK_FALSE(plain.yes);
    CHECK_THROWS(parse("install"));
    CHECK_THROWS(parse("install --verify a/b"));
}

TEST_CASE("remove takes atoms, emerge's --with-bdeps and --yes") {
    const auto remove =
        std::get<egraph::Remove>(parse("remove -y --with-bdeps n a/b =c/d-1").command);
    CHECK(remove.packages == std::vector<std::string>{"a/b", "=c/d-1"});
    CHECK_FALSE(remove.build_deps);
    CHECK(remove.yes);
    const auto plain = std::get<egraph::Remove>(parse("remove a/b").command);
    CHECK(plain.build_deps);
    CHECK_FALSE(plain.yes);
    CHECK_FALSE(parse("remove --dynamic-deps n a/b").dynamic_deps);
    CHECK_THROWS(parse("remove"));
}

TEST_CASE("select and deselect take atoms and --yes") {
    const auto select = std::get<egraph::Select>(parse("select -y a/b @set").command);
    CHECK(select.packages == std::vector<std::string>{"a/b", "@set"});
    CHECK(select.yes);
    const auto deselect = std::get<egraph::Deselect>(parse("deselect a/b").command);
    CHECK(deselect.packages == std::vector<std::string>{"a/b"});
    CHECK_FALSE(deselect.yes);
    CHECK_THROWS(parse("select"));
    CHECK_THROWS(parse("deselect"));
    // deselect follows no dependencies of its own.
    CHECK_THROWS(parse("deselect --dynamic-deps n a/b"));
}

TEST_CASE("emerge runs, and EMERGE_DEFAULT_OPTS is read, under the same roots") {
    egraph::Invocation invocation;
    invocation.builder = "egraph-build";
    const std::vector<std::string> arguments{"--ask=n", "a/b"};
    CHECK(egraph::emerge_command(invocation, arguments) ==
          std::vector<std::string>{"emerge", "--root", "/", "--ask=n", "a/b"});
    CHECK(egraph::emerge_options_command(invocation, "/tmp/out") ==
          std::vector<std::string>{"egraph-build", "--emerge-options", "--output", "/tmp/out",
                                   "--root", "/"});
    invocation.config_root = "/mnt/config";
    CHECK(egraph::emerge_options_command(invocation, "/tmp/out") ==
          std::vector<std::string>{"egraph-build", "--emerge-options", "--output", "/tmp/out",
                                   "--root", "/", "--config-root", "/mnt/config"});
}

TEST_CASE("notices are read, and dispatch-conf runs, under the same roots") {
    CHECK(std::holds_alternative<egraph::NoticesCommand>(parse("notices").command));
    CHECK(parse("--dispatch-conf /bin/true notices").dispatch_conf == "/bin/true");
    egraph::Invocation invocation;
    invocation.builder = "egraph-build";
    CHECK(egraph::notices_command(invocation, "/tmp/out") ==
          std::vector<std::string>{"egraph-build", "--notices", "--output", "/tmp/out", "--root",
                                   "/"});
    CHECK(egraph::dispatch_conf_command(invocation) == std::vector<std::string>{"dispatch-conf"});
    invocation.dispatch_conf = "my-dispatch";
    invocation.root = "/mnt/root";
    invocation.config_root = "/mnt/config";
    invocation.eprefix = "/mnt/prefix";
    // dispatch-conf takes its roots from the environment only.
    CHECK(egraph::dispatch_conf_command(invocation) ==
          std::vector<std::string>{"env", "ROOT=/mnt/root", "PORTAGE_CONFIGROOT=/mnt/config",
                                   "PORTAGE_OVERRIDE_EPREFIX=/mnt/prefix", "my-dispatch"});
}

TEST_CASE("updates and plan write emerge's resume list on request, the actions do not") {
    CHECK(std::get<egraph::Updates>(parse("updates --resume-list /tmp/r").command).resume_list ==
          std::filesystem::path{"/tmp/r"});
    CHECK(
        std::get<egraph::PlanCommand>(parse("plan --resume-list /tmp/r a/b").command).resume_list ==
        std::filesystem::path{"/tmp/r"});
    CHECK_FALSE(std::get<egraph::Updates>(parse("updates").command).resume_list);
    CHECK_THROWS(parse("update --resume-list /tmp/r"));
    CHECK_THROWS(parse("install --resume-list /tmp/r a/b"));
    CHECK_THROWS(parse("sync --resume-list /tmp/r"));
}

TEST_CASE("sync takes updates' options and runs emaint under the same roots") {
    const auto sync = std::get<egraph::Sync>(parse("sync -D --world --verify").command);
    CHECK(sync.deep);
    CHECK(sync.world);
    CHECK(sync.verify);
    CHECK_FALSE(parse("sync --dynamic-deps n").dynamic_deps);
    CHECK_THROWS(parse("sync -y"));
    CHECK(parse("--emaint /bin/true sync").emaint == "/bin/true");
    egraph::Invocation invocation;
    CHECK(egraph::sync_command(invocation) == std::vector<std::string>{"emaint", "sync", "--auto"});
    invocation.emaint = "my-emaint";
    invocation.config_root = "/mnt/config";
    // emaint takes its roots from the environment only.
    CHECK(egraph::sync_command(invocation) ==
          std::vector<std::string>{"env", "PORTAGE_CONFIGROOT=/mnt/config", "my-emaint", "sync",
                                   "--auto"});
}

TEST_CASE("dependency queries take emerge's --dynamic-deps, on by default") {
    for (const auto* command :
         {"deps a/b", "rdeps a/b", "why a/b", "orphans", "broken", "blockers", "affected"}) {
        CHECK(parse(command).dynamic_deps);
        CHECK_FALSE(parse(std::string{command} + " --dynamic-deps n").dynamic_deps);
        CHECK(parse(std::string{command} + " --dynamic-deps y").dynamic_deps);
    }
    CHECK_THROWS_AS(parse("orphans --dynamic-deps maybe"), CLI::ValidationError);
    // Only the queries that follow dependencies take it.
    CHECK_THROWS(parse("soname --dynamic-deps n libc.so.6"));
}

TEST_CASE("deps and rdeps can add what toggled flags would") {
    CHECK_FALSE(std::get<egraph::Deps>(parse("deps a/b").command).possible);
    CHECK(std::get<egraph::Deps>(parse("deps --possible a/b").command).possible);
    CHECK(std::get<egraph::Rdeps>(parse("rdeps --possible a/b").command).possible);
    CHECK_THROWS(parse("why --possible a/b"));

    // Only the ebuilds' dependencies keep their conditionals.
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(parse("rdeps --possible --dynamic-deps n a/b"), out, err) ==
          egraph::Exit::usage);
    CHECK(
        err.str() ==
        "egraph: --possible reads the ebuilds' dependencies, which --dynamic-deps n leaves out\n");
}

TEST_CASE("why takes one atom and emerge's --with-bdeps") {
    const auto why = std::get<egraph::Why>(parse("why --with-bdeps n dev-libs/a").command);
    CHECK(why.package == "dev-libs/a");
    CHECK_FALSE(why.build_deps);
    CHECK_THROWS_AS(parse("why"), CLI::RequiredError);
}

TEST_CASE("the layout is for people on a terminal and lines otherwise") {
    using egraph::ColorDepth;
    egraph::Invocation invocation;
    CHECK_FALSE(egraph::style(invocation).human);
    invocation.terminal = true;
    CHECK(egraph::style(invocation).human);
    CHECK(egraph::style(invocation).color == ColorDepth::palette);
    invocation.truecolor = true;
    CHECK(egraph::style(invocation).color == ColorDepth::truecolor);
    invocation.no_color = true;
    CHECK(egraph::style(invocation).color == ColorDepth::none);
    invocation.color = egraph::ColorMode::always;
    CHECK(egraph::style(invocation).color == ColorDepth::truecolor);
    invocation.layout = egraph::Layout::lines;
    CHECK_FALSE(egraph::style(invocation).human);
    CHECK(egraph::style(invocation).color == ColorDepth::none);

    invocation = parse("--layout human --color never --glyphs unicode stats");
    CHECK(invocation.layout == egraph::Layout::human);
    CHECK(invocation.glyphs == egraph::GlyphSet::unicode);
    CHECK(egraph::style(invocation).human);
    CHECK(egraph::style(invocation).color == ColorDepth::none);
    CHECK_THROWS_AS(parse("--layout fancy stats"), CLI::ValidationError);
    CHECK_THROWS_AS(parse("--glyphs emoji stats"), CLI::ValidationError);
}

TEST_CASE("a choice is taken by name only, and a wrong one names the choices") {
    const auto message = [](const std::string& line) {
        try {
            parse(line);
        } catch (const CLI::ValidationError& e) {
            return std::string{e.what()};
        }
        return std::string{};
    };
    CHECK(message("--glyphs emoji stats") == "--glyphs: emoji is not one of nerd, unicode, ascii");
    CHECK(message("export --format svg") == "--format: svg is not one of dot, json");
    CHECK(message("orphans --with-bdeps maybe") == "--with-bdeps: maybe is not one of y, n");
    // Not the values behind the names.
    CHECK(message("--glyphs 2 stats") == "--glyphs: 2 is not one of nerd, unicode, ascii");
    CHECK(message("orphans --with-bdeps 1") == "--with-bdeps: 1 is not one of y, n");

    CLI::App app;
    egraph::Invocation invocation;
    egraph::configure(app, invocation);
    CHECK(app.help().find("{nerd,unicode,ascii}") != std::string::npos);
}

TEST_CASE("glyphs default to a Nerd Font's in a UTF-8 locale and ASCII otherwise") {
    using egraph::GlyphSet;
    egraph::Invocation invocation;
    CHECK(egraph::style(invocation).glyphs == GlyphSet::ascii);
    invocation.utf8 = true;
    CHECK(egraph::style(invocation).glyphs == GlyphSet::nerd);
    invocation.glyphs = GlyphSet::unicode;
    CHECK(egraph::style(invocation).glyphs == GlyphSet::unicode);
    invocation.utf8 = false;
    CHECK(egraph::style(invocation).glyphs == GlyphSet::unicode);
}

TEST_CASE("the interface needs a terminal, and a build with Notcurses") {
    const auto invocation = parse("tui");
    REQUIRE(std::holds_alternative<egraph::Tui>(invocation.command));
    std::ostringstream out;
    std::ostringstream err;
    const auto exit = egraph::run(invocation, out, err);
    if (egraph::tui::available()) {
        CHECK(exit == egraph::Exit::usage);
        CHECK(err.str() == "egraph: tui: standard output is not a terminal\n");
    } else {
        CHECK(exit == egraph::Exit::not_implemented);
    }
}

TEST_CASE("malformed command lines are rejected") {
    // No command is no error: egraph is interactive then.
    CHECK(std::holds_alternative<std::monostate>(parse("").command));
    CHECK(std::holds_alternative<std::monostate>(parse("--layout lines").command));
    CHECK_THROWS_AS(parse("rdeps"), CLI::RequiredError);
    CHECK_THROWS_AS(parse("export --format svg"), CLI::ValidationError);
    CHECK_THROWS_AS(parse("frobnicate"), CLI::ParseError);
}
