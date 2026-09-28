#pragma once

#include "graph.hpp"
#include "store.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// "parent<TAB>kind<TAB>atom<TAB>child", with "<TAB>any-of" appended for an alternative inside a
// || group.
[[nodiscard]] std::string edge_line(const Store& store, const Edge& edge);

// deps and rdeps: one edge_line per edge, sorted, without duplicates.
[[nodiscard]] std::vector<std::string> edge_lines(const Store& store, std::span<const Edge> edges);

// Consumers (or providers) of a soname: one "cpv<TAB>multilib category" line each, sorted.
[[nodiscard]] std::vector<std::string> soname_users(const Store& store, std::string_view soname,
                                                    bool providers);

// A dependency as portage's paren_enclose renders it: "atom", "|| ( ... )" or "( ... )".
[[nodiscard]] std::string render(const Store& store, std::span<const Node> nodes,
                                 std::size_t index);

// A top-level dependency of a package that nothing installed satisfies.
struct Unsatisfied {
    std::uint32_t package = 0;
    // Index into dep_kinds.
    std::uint32_t kind = 0;
    // Index of the atom or group node in the package's list of that kind.
    std::uint32_t node = 0;
};

// A package's unsatisfied dependencies, in dep_kinds order, then node order.
[[nodiscard]] std::vector<Unsatisfied> unsatisfied(const Store& store, std::uint32_t package);

[[nodiscard]] std::string render(const Store& store, const Unsatisfied& dependency);

// Installed packages with the category and name of an atom in the dependency: what stands where
// it asked for another version or slot. Sorted ids; empty when nothing of the kind is installed.
[[nodiscard]] std::vector<std::uint32_t> installed_instead(const Store& store,
                                                           const Unsatisfied& dependency);

// Each top-level dependency nothing installed satisfies: "cpv<TAB>kind<TAB>dependency", sorted.
[[nodiscard]] std::vector<std::string> broken(const Store& store);

enum class Direction : std::uint8_t { reverse, forward, both };

// Packages within depth dependency edges of roots, following edges in direction; sorted ids.
[[nodiscard]] std::vector<std::uint32_t> neighborhood(const Graph& graph,
                                                      std::span<const std::uint32_t> roots,
                                                      std::uint32_t depth, Direction direction);

// Graphviz of packages and the dependency edges between them, roots filled.
void write_dot(std::ostream& out, const Store& store, const Graph& graph,
               std::span<const std::uint32_t> packages, std::span<const std::uint32_t> roots);

void write_stats(std::ostream& out, const Store& store, const Graph& graph,
                 const std::filesystem::path& path);

} // namespace egraph
