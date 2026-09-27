#include "cli.hpp"

#include <CLI/CLI.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <tuple>

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
    CHECK(invocation.builder == "egraph-build");
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

TEST_CASE("malformed command lines are rejected") {
    CHECK_THROWS_AS(parse(""), CLI::RequiredError);
    CHECK_THROWS_AS(parse("rdeps"), CLI::RequiredError);
    CHECK_THROWS_AS(parse("export --format svg"), CLI::ValidationError);
    CHECK_THROWS_AS(parse("frobnicate"), CLI::ParseError);
}

TEST_CASE("running without a command is a usage error") {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(egraph::Invocation{}, out, err) == egraph::Exit::usage);
}

// Shrinks as commands are implemented.
using Stubs = std::tuple<egraph::Why>;

TEMPLATE_LIST_TEST_CASE("unimplemented commands say so", "", Stubs) {
    egraph::Invocation invocation;
    invocation.command = TestType{};
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::not_implemented);
    CHECK(out.str().empty());
    CHECK(err.str() == "egraph: " + std::string{TestType::name} + ": not implemented\n");
}
