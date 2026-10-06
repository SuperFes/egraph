#pragma once

#include "atom.hpp"
#include "evaluated.hpp"
#include "plan.hpp"
#include "store.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// The replace-slots list: an atom per line, blank lines and "#" comments skipped. An error names
// the line of an atom parse_atom rejects.
[[nodiscard]] std::expected<std::vector<Atom>, std::string>
parse_replace_slots(std::string_view text);

// The list's place under the configuration root, etc/egraph/replace-slots.
[[nodiscard]] std::filesystem::path replace_slots_path(const std::filesystem::path& config_root);
// The list at path; empty when there is none.
[[nodiscard]] std::expected<std::vector<Atom>, std::string>
read_replace_slots(const std::filesystem::path& path);

// Adds to plan an uninstall of each installed package one of atoms matches that a merge new in
// its slot replaces: a root atom naming no slot matches both, and the package is neither
// replaced in its own slot nor uninstalled already. It goes after those merges, unless a root
// atom or a dependency of any kind of what the plan leaves installed (with the merges) needs it:
// satisfied with it, not without.
void replace_slots(const Store& store, const Evaluated& evaluated, std::span<const Atom> atoms,
                   Plan& plan);

} // namespace egraph
