#pragma once

#include "graph.hpp"
#include "store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace egraph {

// What depclean's visibility checks see of an installed package.
struct Masking {
    // Its installed metadata is masked: keywords, package.mask, a license and the like.
    bool masked = false;
    // An ebuild of its version is visible.
    bool visible = true;
};

struct KeepOptions {
    // emerge --with-bdeps: keep what DEPEND and BDEPEND pull in.
    bool build_deps = true;
    // Per installed package, from the evaluated store; empty takes every package as unmasked.
    std::vector<Masking> masking;
    // Per installed package, whether to take it as gone; empty for none.
    std::vector<bool> removed;
    // Per installed package, whether to keep it whatever depends on it, as emerge --depclean
    // keeps every package its arguments do not match; empty for none.
    std::vector<bool> protect;
    // Per root, whether to leave it out, as after emerge --deselect; empty for none.
    std::vector<bool> dropped;
    // Leave the world file's atoms out of the roots, but not those of the sets world_sets names,
    // as emerge --depclean with arguments does.
    bool without_selected = false;
};

// A root atom keeping the installed package it selects.
struct RootPull {
    // Index into Store::roots.
    std::uint32_t root = 0;
    std::uint32_t child = 0;
};

// A runtime dependency of a kept package that nothing installed satisfies, which makes depclean
// refuse to run.
struct Unresolved {
    std::uint32_t parent = 0;
    // Index into dep_kinds.
    std::uint32_t kind = 0;
    // Index of the atom or || node in the parent's list of that kind.
    std::uint32_t node = 0;
};

// What depclean keeps, and through which dependencies it reaches each kept package.
struct Kept {
    std::vector<bool> packages;
    std::vector<RootPull> roots;
    // Edges depclean followed: each atom selects its highest installed match, and each || group
    // one alternative.
    std::vector<Edge> pulls;
    std::vector<Unresolved> unresolved;
};

// Emulates emerge --depclean's graph completion over the installed packages: start from the root
// sets, select the highest installed match of every atom (unmasked ones first, then visible ones),
// follow every dependency kind (build time ones only with build_deps), and resolve || groups
// last, preferring an alternative that is already kept, then the first one installed. A masked
// package that is not visible is unavailable to a || group. Sonames and blockers are not
// followed.
[[nodiscard]] Kept keep(const Store& store, const KeepOptions& options);

// Installed packages depclean would remove, as ids in cpv order.
[[nodiscard]] std::vector<std::uint32_t> orphans(const Kept& kept);

// Why depclean keeps a package: the root that starts the chain, then each dependency in it.
struct Path {
    RootPull root;
    std::vector<Edge> edges;
};

// A shortest chain of kept dependencies from a root to package; nullopt when depclean would
// remove it. Ties go to the earlier root, then to runtime dependencies over build-time ones.
[[nodiscard]] std::optional<Path> why(const Kept& kept, std::uint32_t package);

// "@set<TAB>atom<TAB>cpv" for the root, then an edge_line per dependency.
[[nodiscard]] std::vector<std::string> path_lines(const Store& store, const Path& path);

// "parent<TAB>kind<TAB>dependency" for each unresolved dependency, sorted.
[[nodiscard]] std::vector<std::string> unresolved_lines(const Store& store, const Kept& kept);

} // namespace egraph
