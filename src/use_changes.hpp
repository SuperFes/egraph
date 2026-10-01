#pragma once

#include "evaluated.hpp"
#include "store.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>

namespace egraph {

// USE changes to one candidate, as autounmask proposes them for package.use.
struct UseChange {
    // Index into Evaluated::candidates.
    std::uint32_t candidate = 0;
    // Each changed flag and the state it takes.
    std::map<std::string, bool, std::less<>> flags;
};

// evaluated with each changed candidate built with its USE changed: its USE is its own with the
// flags set as changed, and its dependencies are its tokens reduced under that USE
// (reduce_dependencies), matched against the installed packages as the builder matches them.
// Candidate indices, and every string id already in evaluated, stay as they are.
[[nodiscard]] Evaluated with_use_changes(Evaluated evaluated, const Store& installed,
                                         std::span<const UseChange> changes);

} // namespace egraph
