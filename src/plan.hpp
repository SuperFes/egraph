#pragma once

#include "evaluated.hpp"
#include "query.hpp"
#include "store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace egraph {

// A package the plan holds: installed, or a candidate it merges.
struct Member {
    bool candidate = false;
    // Installed package id, or index into Evaluated::candidates.
    std::uint32_t index = 0;
    auto operator<=>(const Member&) const = default;
};

// A dependency of a member that stands in a merge's way, or pulls one in.
struct Reason {
    Member member;
    // The atom as the member's dependencies print it.
    std::string atom;
    auto operator<=>(const Reason&) const = default;
};

// A package the plan merges.
struct Merge {
    // Index into Evaluated::candidates.
    std::uint32_t candidate = 0;
    // The installed package it replaces, with the update's kind and flags; none for a package
    // new in its slot.
    std::optional<std::uint32_t> replaces;
    UpdateKind kind = UpdateKind::upgrade;
    std::string flags;
    // For a new package, the dependency that first pulled it in.
    std::optional<Reason> pulled_by;
};

// An installed package whose pending update the plan leaves out, or replaces with an earlier
// version.
struct HeldBack {
    std::uint32_t package = 0;
    PendingUpdate wanted;
    // What rejects it, sorted: dependents' atoms that the update would leave unsatisfied, and
    // the target's own dependencies (or those of what it pulls in) that nothing can satisfy.
    std::vector<Reason> reasons;
};

struct Plan {
    // Replacements in the installed packages' order, then new packages by cpv.
    std::vector<Merge> merges;
    // In the installed packages' order.
    std::vector<HeldBack> held;
};

// What emerge -uD would merge for the installed packages in scope (every one when scope is
// empty), with store's dependencies (read with or without dynamic deps) for the installed ones
// and the candidates' own for what it merges:
// - each package's pending update, or the best visible version in its slot that no member's
//   dependency rejects; held back when none is left;
// - whatever a member's dependencies need that nothing in the plan satisfies: the best visible
//   version that matches, new in its slot, the first alternative of a || that can be satisfied
//   so. A merge whose own dependencies cannot be satisfied falls back in turn.
// Slot operators do not hold an update back; blockers are not weighed.
[[nodiscard]] Plan plan_updates(const Store& store, const Evaluated& evaluated,
                                UseRebuilds rebuilds, const std::vector<bool>& scope = {});

} // namespace egraph
