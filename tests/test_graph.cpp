#include "graph.hpp"
#include "query.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

using egraph::NodeType;
using egraph::test::Bytes;

namespace {

egraph::Store decoded(const std::vector<std::byte>& bytes) {
    auto store = egraph::decode(bytes);
    REQUIRE(store.has_value());
    return std::move(*store);
}

// a-1 RDEPEND: || ( dev-libs/b dev-libs/missing ) !app-misc/old, b-1 installed.
egraph::Store sample() {
    return decoded(egraph::test::fresh_sample());
}

std::vector<egraph::Node> rdepend(const egraph::Store& store, std::uint32_t package) {
    const auto nodes = store.nodes_in(store.packages.at(package).deps.at(4));
    return {nodes.begin(), nodes.end()};
}

egraph::Node node(NodeType type, std::uint32_t parent, std::uint32_t atom, bool matched) {
    return {.type = type,
            .parent = parent,
            .atom = atom,
            .matches = {.first = 0, .count = matched ? 1U : 0U}};
}

} // namespace

TEST_CASE("choices and satisfaction follow the tree") {
    const auto store = sample();
    const auto nodes = rdepend(store, 0);
    CHECK(egraph::choices(nodes) == std::vector<bool>{false, true, true, false});
    CHECK(egraph::satisfied(nodes) == std::vector<bool>{true, true, false, true});
}

TEST_CASE("an any-of under an all-of") {
    // || ( ( || ( x y ) z ) ), with only y and z installed.
    const std::vector<egraph::Node> nodes{
        node(NodeType::any_of, egraph::no_parent, 0, false),
        node(NodeType::all_of, 0, 0, false),
        node(NodeType::any_of, 1, 0, false),
        node(NodeType::atom, 2, 1, false),
        node(NodeType::atom, 2, 2, true),
        node(NodeType::atom, 1, 3, true),
    };
    CHECK(egraph::choices(nodes) == std::vector<bool>{false, true, true, true, true, true});
    CHECK(egraph::satisfied(nodes) == std::vector<bool>{true, true, true, false, true, true});
    CHECK(egraph::satisfied(std::vector<egraph::Node>{
              node(NodeType::any_of, egraph::no_parent, 0, false)}) == std::vector<bool>{true});
}

TEST_CASE("edges run both ways") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    const egraph::Edge edge{.parent = 0, .child = 1, .kind = 4, .atom = 7, .choice = true};
    CHECK(std::vector(graph.deps(0).begin(), graph.deps(0).end()) ==
          std::vector<egraph::Edge>{edge});
    CHECK(std::vector(graph.rdeps(1).begin(), graph.rdeps(1).end()) ==
          std::vector<egraph::Edge>{edge});
    CHECK(graph.deps(1).empty());
    CHECK(graph.rdeps(0).empty());

    std::ostringstream out;
    egraph::write_edges(out, store, graph.deps(0));
    CHECK(out.str() == "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1\tany-of\n");
}

TEST_CASE("arguments name a cpv or every version of a cp") {
    const auto store = sample();
    CHECK(egraph::resolve(store, "app-misc/a-1") == std::vector<std::uint32_t>{0});
    CHECK(egraph::resolve(store, "dev-libs/b") == std::vector<std::uint32_t>{1});
    CHECK(egraph::resolve(store, "dev-libs/b-2").empty());
    CHECK(egraph::resolve(store, ">=dev-libs/b-1").empty());
}

TEST_CASE("sonames list consumers or providers") {
    const auto store = sample();
    CHECK(egraph::soname_users(store, "libb.so.1", false) ==
          std::vector<std::string>{"app-misc/a-1\tx86_64"});
    CHECK(egraph::soname_users(store, "libb.so.1", true) ==
          std::vector<std::string>{"dev-libs/b-1\tx86_64"});
    CHECK(egraph::soname_users(store, "libnone.so", false).empty());
}

TEST_CASE("broken renders each unsatisfied top-level dependency as portage does") {
    CHECK(egraph::broken(sample()).empty());
    // || ( dev-libs/missing ( dev-libs/b dev-libs/missing ) ) !app-misc/old, nothing matching.
    const auto store = decoded(egraph::test::with_rdepend(6, [](Bytes& b) {
        b.varints({1, 0, 0}).list({});
        b.varints({0, 1, 14}).list({});
        b.varints({2, 1, 0}).list({});
        b.varints({0, 3, 7}).list({});
        b.varints({0, 3, 14}).list({});
        b.varints({3, 0, 13}).list({});
    }));
    CHECK(egraph::broken(store) ==
          std::vector<std::string>{"app-misc/a-1\tRDEPEND\t"
                                   "|| ( dev-libs/missing ( dev-libs/b dev-libs/missing ) )"});
}

TEST_CASE("neighborhoods follow the chosen direction to the chosen depth") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    const std::vector<std::uint32_t> b{1};
    CHECK(egraph::neighborhood(graph, b, 1, egraph::Direction::reverse) ==
          std::vector<std::uint32_t>{0, 1});
    CHECK(egraph::neighborhood(graph, b, 1, egraph::Direction::forward) ==
          std::vector<std::uint32_t>{1});
    CHECK(egraph::neighborhood(graph, b, 0, egraph::Direction::both) ==
          std::vector<std::uint32_t>{1});
    const std::vector<std::uint32_t> a{0};
    CHECK(egraph::neighborhood(graph, a, 5, egraph::Direction::forward) ==
          std::vector<std::uint32_t>{0, 1});
}

TEST_CASE("dot output") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    std::ostringstream out;
    const std::vector<std::uint32_t> all{0, 1};
    const std::vector<std::uint32_t> roots{1};
    egraph::write_dot(out, store, graph, all, roots);
    CHECK(out.str() == "digraph \"egraph\" {\n"
                       "\trankdir=LR;\n"
                       "\tnode [shape=box, fontname=\"monospace\", fontsize=10];\n"
                       "\tedge [fontname=\"monospace\", fontsize=8];\n"
                       "\t\"app-misc/a-1\";\n"
                       "\t\"dev-libs/b-1\" [style=filled, fillcolor=\"#ffd966\"];\n"
                       "\t\"app-misc/a-1\" -> \"dev-libs/b-1\" [label=\"RDEPEND\", style=dashed];\n"
                       "}\n");
}

TEST_CASE("stats") {
    const auto store = sample();
    std::ostringstream out;
    egraph::write_stats(out, store, egraph::build_graph(store), "/s");
    CHECK(out.str() == "store: /s\n"
                       "eroot: /\n"
                       "built: 1970-01-01T00:00:00Z\n"
                       "packages: 2\n"
                       "dependency nodes: 4\n"
                       "edges: 1\n"
                       "unsatisfied: 0\n"
                       "sonames: 1\n"
                       "inputs: 0\n");
}
