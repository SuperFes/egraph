#pragma once

#include "store.hpp"

#include <compare>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace egraph {

// An installed package depending on another through one atom of one kind, as the oracle's Edge.
struct Edge {
    std::uint32_t parent = 0;
    std::uint32_t child = 0;
    // Index into dep_kinds.
    std::uint32_t kind = 0;
    // String id of the atom.
    std::uint32_t atom = 0;
    // The atom is an alternative inside a || group.
    bool choice = false;

    auto operator<=>(const Edge&) const = default;
};

// Edge indexes over a Store, holding ids only; every query takes the Store it was built from.
struct Graph {
    std::vector<Edge> forward;
    std::vector<Range> forward_of;
    std::vector<Edge> reverse;
    std::vector<Range> reverse_of;

    [[nodiscard]] std::span<const Edge> deps(std::uint32_t package) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const Edge> rdeps(std::uint32_t package) const EGRAPH_LIFETIMEBOUND;
};

[[nodiscard]] Graph build_graph(const Store& store);

// Per node of one dependency list, whether it sits inside a || group.
[[nodiscard]] std::vector<bool> choices(std::span<const Node> nodes);

// Per node of one dependency list, whether installed packages satisfy it. Blockers always do:
// they are constraints, not dependencies.
[[nodiscard]] std::vector<bool> satisfied(std::span<const Node> nodes);

// Package ids an argument names: the exact cpv, or every installed version of a cp.
[[nodiscard]] std::vector<std::uint32_t> resolve(const Store& store, std::string_view argument);

} // namespace egraph
