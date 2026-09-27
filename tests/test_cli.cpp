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
    CHECK(rdeps->package == "dev-libs/openssl");
    CHECK_FALSE(invocation.store.has_value());
    CHECK_FALSE(invocation.no_refresh);
}

TEST_CASE("global options precede the command") {
    const auto invocation = parse("--store /tmp/x.egraph --no-refresh broken");
    CHECK(std::holds_alternative<egraph::Broken>(invocation.command));
    CHECK(invocation.store == std::filesystem::path{"/tmp/x.egraph"});
    CHECK(invocation.no_refresh);
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

TEST_CASE("malformed command lines are rejected") {
    CHECK_THROWS_AS(parse(""), CLI::RequiredError);
    CHECK_THROWS_AS(parse("rdeps"), CLI::RequiredError);
    CHECK_THROWS_AS(parse("rdeps a b"), CLI::ExtrasError);
    CHECK_THROWS_AS(parse("export --format svg"), CLI::ValidationError);
    CHECK_THROWS_AS(parse("frobnicate"), CLI::ParseError);
}

TEST_CASE("running without a command is a usage error") {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(egraph::Invocation{}, out, err) == egraph::Exit::usage);
}

// Shrinks as commands are implemented.
using Stubs =
    std::tuple<egraph::Deps, egraph::Rdeps, egraph::Why, egraph::Soname, egraph::Broken,
               egraph::Orphans, egraph::Export, egraph::Stats, egraph::Rebuild, egraph::Check>;

TEMPLATE_LIST_TEST_CASE("unimplemented commands say so", "", Stubs) {
    egraph::Invocation invocation;
    invocation.command = TestType{};
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::not_implemented);
    CHECK(out.str().empty());
    CHECK(err.str() == "egraph: " + std::string{TestType::name} + ": not implemented\n");
}
