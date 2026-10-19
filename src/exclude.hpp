#pragma once

// Packages depclean never removes, as emerge --depclean --exclude keeps them.

#include "atom.hpp"
#include "store.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// One exclusion as given, and the atom it reads as.
struct Exclusion {
    std::string text;
    Atom atom;
};

// Each whitespace-separated atom of values, as emerge's --exclude takes them: package names,
// with or without a category, and slot atoms, wildcards allowed; no operator, USE dependency,
// repository or blocker.
[[nodiscard]] std::expected<std::vector<Exclusion>, std::string>
parse_exclusions(std::span<const std::string> values);

// The depclean-exclude list: atoms as parse_exclusions takes them, blank lines and "#" comments
// skipped. An error names the line.
[[nodiscard]] std::expected<std::vector<Exclusion>, std::string>
parse_exclude_list(std::string_view text);

// The list's place under the configuration root, etc/egraph/depclean-exclude.
[[nodiscard]] std::filesystem::path depclean_exclude_path(const std::filesystem::path& config_root);
// The list at path; empty when there is none.
[[nodiscard]] std::expected<std::vector<Exclusion>, std::string>
read_exclude_list(const std::filesystem::path& path);

// Whether the exclusion matches the installed package.
[[nodiscard]] bool excludes(const Store& store, const Package& pkg, const Exclusion& exclusion);

} // namespace egraph
