#pragma once

// @selected's own atoms, the world file's and world_sets', as select and deselect change them.

#include "depclean.hpp"
#include "store.hpp"

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// The world file's atoms: @selected's roots that no world_sets set gives.
[[nodiscard]] std::set<std::string, std::less<>> world_atoms(const Store& store);

// "atom<TAB>selected" for each atom only after has, "atom<TAB>deselected" for each only before
// has, by atom.
[[nodiscard]] std::vector<std::string>
selection_changes(const std::set<std::string, std::less<>>& before,
                  const std::set<std::string, std::less<>>& after);

// What emerge --pretend --deselect would remove from the world file, and from world_sets as
// "@set", from its "Would remove" lines, sorted.
[[nodiscard]] std::vector<std::string> parse_deselect(std::string_view output);

// What deselecting atoms (parse_deselect's) leaves: "atom<TAB>deselect" for each, then
// "cpv<TAB>orphan" for each package depclean, given options as for keep(), would remove after it
// but not before, by cpv.
[[nodiscard]] std::vector<std::string>
deselect_lines(const Store& store, const KeepOptions& options, std::span<const std::string> atoms);

} // namespace egraph
