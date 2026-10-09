#include "helpers.hpp"
#include "session.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using egraph::test::Bytes;
using egraph::test::TempDir;
using egraph::test::write_bytes;

namespace {

// Stores at dir/installed.egraph that never refresh: an answer from a stale one is a warning.
egraph::Invocation at(const std::filesystem::path& dir) {
    egraph::Invocation invocation;
    invocation.store = dir / "installed.egraph";
    invocation.builder = "/nonexistent/egraph-build";
    invocation.no_refresh = true;
    return invocation;
}

void write_fresh(const std::filesystem::path& dir, bool evaluated = true) {
    write_bytes(dir / "installed.egraph", egraph::test::fresh_sample());
    if (evaluated) {
        write_bytes(dir / "installed.evaluated.egraph",
                    egraph::test::evaluated_with_section(2, Bytes{}.varint(0)));
    }
}

template <class T> const T* address(const egraph::Loaded<T>& loaded) {
    REQUIRE(loaded.has_value());
    return &loaded->get();
}

} // namespace

TEST_CASE("a session loads the installed store without the evaluated one") {
    const TempDir dir;
    write_fresh(dir.path(), false);
    std::ostringstream warnings;
    egraph::Session session{at(dir.path()), warnings};
    const auto installed = session.installed();
    REQUIRE(installed.has_value());
    CHECK(installed->get().packages.size() == 2);
    CHECK(session.used() == dir.path() / "installed.egraph");
    CHECK_FALSE(session.stores().has_value());
    CHECK_FALSE(session.dependencies(true).has_value());
    CHECK(session.dependencies(false).has_value());
    CHECK(warnings.str().empty());
}

TEST_CASE("a session loads each piece once") {
    const TempDir dir;
    write_fresh(dir.path());
    std::ostringstream warnings;
    egraph::Session session{at(dir.path()), warnings};
    const auto* installed = address(session.installed());
    CHECK(address(session.installed()) == installed);
    const auto* stores = address(session.stores());
    CHECK(address(session.stores()) == stores);
    // The installed store stays the one first loaded.
    CHECK(address(session.installed()) == installed);
    CHECK(address(session.dependencies(false)) == installed);
    const auto* dynamic = address(session.dependencies(true));
    CHECK(dynamic != installed);
    CHECK(address(session.dependencies(true)) == dynamic);
    // The evaluated store's a-1 depends on dev-libs/b:= where the installed one has four nodes.
    CHECK(dynamic->nodes_in(dynamic->packages.at(0).deps.at(4)).size() == 1);
    CHECK(installed->nodes_in(installed->packages.at(0).deps.at(4)).size() == 4);
    const auto* graph = address(session.graph(true));
    CHECK(address(session.graph(true)) == graph);
    CHECK(graph->deps(0).size() == 1);

    const auto* depclean = address(session.depclean(true, true));
    CHECK(address(session.depclean(true, true)) == depclean);
    CHECK(address(session.depclean(false, true)) != depclean);
    CHECK(&depclean->store.get() == dynamic);
    // Without dynamic deps, the store whose packages line up with the evaluated store's masks.
    CHECK(&address(session.depclean(true, false))->store.get() == &stores->installed);
    CHECK(warnings.str().empty());
}

TEST_CASE("a session warns once about a stale store") {
    const TempDir dir;
    // The sample records an input the system does not have.
    write_bytes(dir.path() / "installed.egraph",
                egraph::test::assemble(egraph::test::sample_sections()));
    std::ostringstream warnings;
    egraph::Session session{at(dir.path()), warnings};
    CHECK(session.installed().has_value());
    CHECK(session.installed().has_value());
    CHECK(session.dependencies(false).has_value());
    const auto text = warnings.str();
    CHECK(text.starts_with("egraph: warning: answering from a stale store ("));
    CHECK(text.find("egraph: warning", 1) == std::string::npos);
}

TEST_CASE("a session reports what it cannot load") {
    const TempDir dir;
    std::ostringstream warnings;
    egraph::Session session{at(dir.path()), warnings};
    const auto installed = session.installed();
    REQUIRE_FALSE(installed.has_value());
    CHECK(installed.error().find("installed.egraph") != std::string::npos);
    CHECK_FALSE(session.graph(false).has_value());
    CHECK_FALSE(session.depclean(true, true).has_value());
}

TEST_CASE("a session answers from the stores it adopts") {
    const TempDir dir;
    write_fresh(dir.path());
    std::ostringstream warnings;
    egraph::Session session{at(dir.path()), warnings};
    const auto shared = session.shared_stores();
    REQUIRE(shared.has_value());
    CHECK(address(session.stores()) == shared->get());
    CHECK(&address(session.depclean(true, false))->store.get() == &(*shared)->installed);

    const auto adopted = std::make_shared<const egraph::Stores>(**shared);
    session.adopt(adopted, dir.path() / "other.egraph");
    CHECK(session.used() == dir.path() / "other.egraph");
    CHECK(address(session.stores()) == adopted.get());
    CHECK(address(session.installed()) == &adopted->installed);
    CHECK(address(session.dependencies(false)) == &adopted->installed);
    CHECK(&address(session.depclean(true, false))->store.get() == &adopted->installed);
    // The old stores live on with whoever shares them.
    CHECK((*shared)->installed.packages.size() == 2);
}

TEST_CASE("a store in another format is put down to another version of egraph or its builder") {
    const egraph::StoreError mismatch{
        .message = "/s/installed.egraph: format version 4, expected 5",
        .mismatch = egraph::FormatMismatch{
            .kind = "an egraph store", .found = 4, .expected = 5, .path = "/s/installed.egraph"}};
    CHECK(egraph::built_store_error("egraph-build", mismatch) ==
          "egraph and egraph-build are from different versions: egraph-build wrote "
          "/s/installed.egraph as an egraph store of format 4, but this egraph reads format 5; "
          "install both from the same release");
    CHECK(egraph::stored_store_error(mismatch) ==
          "/s/installed.egraph is an egraph store of format 4, from another egraph version, but "
          "this egraph reads format 5; egraph rebuild writes it anew");
    const egraph::StoreError other{.message = "/s/installed.egraph: truncated header"};
    CHECK(egraph::built_store_error("egraph-build", other) == other.message);
    CHECK(egraph::stored_store_error(other) == other.message);
}

TEST_CASE("a session without refreshing says how to replace a store from another version") {
    const TempDir dir;
    write_bytes(dir.path() / "installed.egraph",
                egraph::test::assemble(egraph::test::sample_sections(), 3));
    std::ostringstream warnings;
    egraph::Session session{at(dir.path()), warnings};
    const auto installed = session.installed();
    REQUIRE_FALSE(installed.has_value());
    CHECK(installed.error().ends_with("egraph rebuild writes it anew"));
}

TEST_CASE("untried stores are loaded without trying the lines") {
    const TempDir dir;
    write_fresh(dir.path());
    auto invocation = at(dir.path());
    invocation.config_root = dir.path();
    invocation.what_if = {
        {.file = egraph::WhatIfLine::File::env, .atom = "app-misc/a", .tokens = {"missing.conf"}}};
    std::ostringstream warnings;
    egraph::Session session{invocation, warnings};
    CHECK(session.untried_stores().has_value());
    REQUIRE_FALSE(session.stores().has_value());
    CHECK(session.stores().error() == "env/missing.conf: no such env file");
}

namespace {

egraph::Exit save(const egraph::Invocation& invocation, std::string& out, std::string& err) {
    std::ostringstream printed;
    std::ostringstream problems;
    const auto exit = egraph::run(invocation, printed, problems);
    out = printed.str();
    err = problems.str();
    return exit;
}

} // namespace

TEST_CASE("save writes the lines to egraph's files under the configuration root") {
    const TempDir dir;
    write_fresh(dir.path());
    auto invocation = at(dir.path());
    invocation.config_root = dir.path();
    invocation.layout = egraph::Layout::lines;
    invocation.command = egraph::Save{};
    std::string out;
    std::string err;
    CHECK(save(invocation, out, err) == egraph::Exit::usage);
    CHECK(err == "egraph: save: nothing to save; give the lines with --use and --env\n");

    invocation.what_if = {
        {.file = egraph::WhatIfLine::File::use, .atom = "app-misc/a", .tokens = {"x", "-y"}},
        {.file = egraph::WhatIfLine::File::use, .atom = "*/*", .tokens = {"-nls"}}};
    REQUIRE(save(invocation, out, err) == egraph::Exit::ok);
    const auto file = dir.path() / "etc/portage/package.use/egraph";
    CHECK(out == file.string() + "\tapp-misc/a x -y\n" + file.string() + "\t*/* -nls\n");
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().ends_with("\napp-misc/a x -y\n*/* -nls\n"));

    // A line naming an env file the configuration lacks is not saved.
    invocation.what_if = {
        {.file = egraph::WhatIfLine::File::env, .atom = "app-misc/a", .tokens = {"missing.conf"}}};
    CHECK(save(invocation, out, err) == egraph::Exit::failure);
    CHECK(err == "egraph: save: env/missing.conf: no such env file\n");
    CHECK_FALSE(std::filesystem::exists(dir.path() / "etc/portage/package.env"));
}

TEST_CASE("a shell line saves the lines it gives, though they choose no stores") {
    const TempDir dir;
    write_fresh(dir.path());
    auto invocation = at(dir.path());
    invocation.config_root = dir.path();
    std::istringstream in{"--use 'app-misc/a x' save\n"};
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::shell(invocation, in, out, err, false) == egraph::Exit::ok);
    CHECK(err.str().find("chooses the stores") == std::string::npos);
    CHECK(std::filesystem::exists(dir.path() / "etc/portage/package.use/egraph"));
}
