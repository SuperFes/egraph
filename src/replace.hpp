#pragma once

#include "atom.hpp"
#include "evaluated.hpp"
#include "plan.hpp"
#include "store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
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

// The running kernel's release, from proc's sys/kernel/osrelease; none if it cannot be read.
[[nodiscard]] std::optional<std::string>
kernel_release(const std::filesystem::path& proc = "/proc");

// The kernel sources the release's modules were built from: where lib/modules/<release>/build
// (else source) under root points, as a path on that system; none without either link.
[[nodiscard]] std::optional<std::filesystem::path> kernel_sources(const std::filesystem::path& root,
                                                                  std::string_view release);

// The installed packages plan uninstalls as replaced slots, by id.
[[nodiscard]] std::vector<std::uint32_t> replaced_slots(const Plan& plan);

// What egraph-build --kernel-sources writes: each cpv's kernel source directories.
using KernelSources = std::map<std::string, std::vector<std::string>, std::less<>>;
[[nodiscard]] std::expected<KernelSources, std::string> parse_kernel_sources(std::string_view text);

// Of packages, those owning the running kernel's sources, as sources has them.
[[nodiscard]] std::vector<std::uint32_t>
running_kernel_owners(const Store& store, std::span<const std::uint32_t> packages,
                      const KernelSources& sources, const std::filesystem::path& running);

// Adds to plan an uninstall of each installed package one of atoms matches that a merge new in
// its slot replaces: a root atom naming no slot matches both, and the package is neither
// replaced in its own slot nor uninstalled already. It goes after those merges, unless a root
// atom or a dependency of any kind of what the plan leaves installed (with the merges) needs it:
// satisfied with it, not without. The packages kept are never replaced.
void replace_slots(const Store& store, const Evaluated& evaluated, std::span<const Atom> atoms,
                   std::span<const std::uint32_t> kept, Plan& plan);

} // namespace egraph
