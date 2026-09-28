#pragma once

#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace egraph {

inline constexpr std::uint32_t evaluated_format_version = 3;

// Where an installed package's dependency strings came from under --dynamic-deps=y.
enum class DepSource : std::uint8_t { ebuild, vdb, moved };

// An installed package's dependencies as emerge reads them by default.
struct Dependencies {
    // String id; the installed package's at the same index.
    std::uint32_t cpv = 0;
    DepSource source = DepSource::vdb;
    // String id of the EAPI the strings were read with.
    std::uint32_t eapi = 0;
    // In Evaluated::pairs.
    Range errors;
    // In Evaluated::nodes, one per entry of dep_kinds; matches are installed package ids.
    std::array<Range, dep_kinds.size()> deps;
    // In Evaluated::possible.
    Range possible;
    // An ebuild of the same version is visible, as emerge requires of an installed package it
    // keeps when an ebuild in its slot is visible.
    bool visible = true;
    // Its installed metadata is masked (keywords, package.mask, license and the like) under
    // --dynamic-deps=y, and under =n: depclean passes over a masked package unless visible.
    bool masked = false;
    bool vdb_masked = false;
    // Index into Evaluated::candidates: the best visible version in its slot, when emerge -u
    // would replace the package with it (rebuild empty) or --newuse would rebuild it from it.
    std::optional<std::uint32_t> target;
    // String ids in Evaluated::ids: the flags --newuse rebuilds it for, as emerge shows them
    // ("flag*", "-flag%", "(-flag%*)"); those with a * are --changed-use's.
    Range rebuild;
};

// A dependency the ebuild would add with flags toggled that the installed build left as they are.
struct Possible {
    // Index into dep_kinds.
    std::uint32_t kind = 0;
    // String id.
    std::uint32_t atom = 0;
    // Inside a || group.
    bool choice = false;
    // Installed package ids in Evaluated::ids.
    Range matches;
    // String ids in Evaluated::ids, the fewest toggles that add it: "flag" turns a flag on,
    // "-flag" turns one off.
    Range flags;
};

// One version of an installed cp in one repository.
struct Candidate {
    // String ids.
    std::uint32_t cp = 0;
    std::uint32_t cpv = 0;
    std::uint32_t repo = 0;
    std::uint32_t slot = 0;
    std::uint32_t sub_slot = 0;
    // String ids in Evaluated::ids: the flags it would be built with now, its IUSE, and why it
    // is masked.
    Range use;
    Range iuse;
    Range reasons;

    [[nodiscard]] bool visible() const { return reasons.count == 0; }
};

struct EvaluatedMeta {
    std::string egraph_version;
    std::string portage_version;
    std::string eroot;
    std::uint64_t build_time_ns = 0;
    // The build start of the installed store whose package ids this one uses.
    std::uint64_t installed_build_time_ns = 0;
};

// A decoded evaluated store. Every id and Range in it was checked against its table.
struct Evaluated : Tables {
    EvaluatedMeta meta;
    std::vector<Input> inputs;
    std::vector<Dependencies> packages;
    std::vector<Possible> possible;
    std::vector<Candidate> candidates;

    [[nodiscard]] std::span<const Possible> possible_in(Range range) const EGRAPH_LIFETIMEBOUND {
        return std::span{possible}.subspan(range.first, range.count);
    }
};

// Rejects anything that does not follow docs/store-format.md exactly.
[[nodiscard]] std::expected<Evaluated, StoreError>
decode_evaluated(std::span<const std::byte> data);

// Decodes path and checks that its packages are installed's, index for index.
[[nodiscard]] std::expected<Evaluated, StoreError> load_evaluated(const std::filesystem::path& path,
                                                                  const Store& installed);

// An installed store and the evaluated store built with it.
struct Stores {
    Store installed;
    Evaluated evaluated;
};

// The installed store at path and the evaluated store beside it.
[[nodiscard]] std::expected<Stores, StoreError> load_stores(const std::filesystem::path& path);

// The installed store with every package's dependency trees and dependency parse errors replaced
// by the evaluated store's, as emerge reads them under --dynamic-deps=y. Everything else, and
// every package id, stays the installed store's.
[[nodiscard]] Store with_dynamic_deps(Store installed, const Evaluated& evaluated);

// Beside the installed store: its last extension replaced by .evaluated.egraph.
[[nodiscard]] std::filesystem::path evaluated_store_path(const std::filesystem::path& installed);

} // namespace egraph
