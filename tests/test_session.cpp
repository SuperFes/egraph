#include "helpers.hpp"
#include "session.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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
    CHECK(address(session.graph(false)) != graph);
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
