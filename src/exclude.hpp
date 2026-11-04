#pragma once

// Packages depclean never removes, as emerge --depclean --exclude keeps them.

#include "atom.hpp"
#include "evaluated.hpp"
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

// Whether the exclusion matches the candidate, whatever its USE.
[[nodiscard]] bool excludes(const Evaluated& evaluated, const Candidate& candidate,
                            const Exclusion& exclusion);

// Whether each installed package, by id, is excluded; empty without exclusions.
[[nodiscard]] std::vector<bool> excluded_packages(const Store& installed,
                                                  std::span<const Exclusion> exclusions);

// evaluated as emerge --exclude leaves it: each candidate an exclusion matches masked ("exclude
// option"), and no installed package one matches updated or rebuilt. Candidate indices stay as
// they are.
[[nodiscard]] Evaluated with_exclusions(Evaluated evaluated, const Store& installed,
                                        std::span<const Exclusion> exclusions);

} // namespace egraph
