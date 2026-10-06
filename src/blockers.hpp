#pragma once

#include "evaluated.hpp"
#include "plan.hpp"
#include "query.hpp"
#include "store.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// blockers: "holder<TAB>kind<TAB>atom<TAB>blocked" for each blocker in an installed package's
// dependencies and each installed package it matches, sorted. Of the packages named, every blocker
// they hold (blocked left empty when it matches nothing) and every blocker matching one of them;
// of none, the blockers that match something installed.
[[nodiscard]] std::vector<std::string> blocker_lines(const Store& store,
                                                     std::span<const std::uint32_t> named);

// Sets plan's uninstalls and blocks from the blockers between what it leaves installed, as
// emerge's _validate_blockers: an installed package's run-time ones (store's, read as the plan
// read them) and a merge's of every kind. A blocker in a merge's own slot is ignored unless
// strong; one on a version a merge replaces needs nothing, unless strong and the replacing merge
// holds it. Otherwise a merge's blocker on a package staying installed uninstalls that package,
// and an installed package's blocker on a merge uninstalls the holder. Neither when strong on
// the running root, nor when emerge's completed graph needs the package: reached from the root
// sets, emerge's arguments (every installed package without targets.roots) or a merge, through
// dependencies of every kind, each atom taking the best version left that matches and a || its
// first alternative left satisfied. What cannot be resolved so, and a blocker between two
// merges, is a block. Sets plan's masked from the same completed graph.
void weigh_blockers(const Store& store, const Evaluated& evaluated, const Targets& targets,
                    Plan& plan);

} // namespace egraph
