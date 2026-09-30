#pragma once

#include "store.hpp"

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// A node of a dependency string reduced under a USE, as the builder's node lists hold it.
struct ReducedNode {
    NodeType type = NodeType::atom;
    std::uint32_t parent = no_parent;
    // The atom as portage prints it once its USE conditionals are evaluated; empty for groups.
    std::string text;
    bool operator==(const ReducedNode&) const = default;
};

// A dependency string, as its split() tokens, reduced as portage's use_reduce does (not flat,
// no operator conversion) under the enabled flags, then laid out as the builder lays out its
// node lists. The tokens must parse, as a visible ebuild's do; empty_true is the EAPI's
// empty_groups_always_true.
[[nodiscard]] std::vector<ReducedNode>
reduce_dependencies(std::span<const std::string_view> tokens,
                    const std::set<std::string_view>& enabled, bool empty_true);

// The atom with its USE dependencies' conditionals evaluated under the enabled flags, as
// portage's Atom.evaluate_conditionals prints it; unchanged without conditionals.
[[nodiscard]] std::string evaluate_use_conditionals(std::string_view atom,
                                                    const std::set<std::string_view>& enabled);

} // namespace egraph
