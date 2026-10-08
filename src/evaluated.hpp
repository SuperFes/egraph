#pragma once

#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

inline constexpr std::uint32_t evaluated_format_version = 11;

// Where an installed package's dependency strings came from under --dynamic-deps=y.
enum class DepSource : std::uint8_t { ebuild, vdb, moved };

// How emerge sees an installed package: visible, not (package.mask, an invalid string, an
// unsupported EAPI), or not and masked by LICENSE, which it warns of whatever its graph holds.
enum class Hidden : std::uint8_t { visible, hidden, license };

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
    // String ids in Evaluated::ids: why it is masked, where masked is computed and true, as
    // emerge's warning about masked installed packages words it; the same for vdb_masked.
    Range mask_reasons;
    Range vdb_mask_reasons;
    // String ids: the package.mask file and comment, when package.mask is among the reasons.
    std::uint32_t mask_file = 0;
    std::uint32_t mask_comment = 0;
    // How emerge sees it, where masked is computed, under --dynamic-deps=y and =n.
    Hidden hidden = Hidden::visible;
    Hidden vdb_hidden = Hidden::visible;
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

// One version of a cp in one repository: an installed cp, or one that dependencies reach where
// nothing installed satisfies them.
struct Candidate {
    // String ids.
    std::uint32_t cp = 0;
    std::uint32_t cpv = 0;
    std::uint32_t repo = 0;
    std::uint32_t slot = 0;
    std::uint32_t sub_slot = 0;
    // String ids in Evaluated::ids: the flags it would be built with now, its IUSE, those of its
    // IUSE the profile masks or forces, and why it is masked.
    Range use;
    Range iuse;
    Range forced;
    Range reasons;
    // In Evaluated::pairs: dependency strings portage could not parse.
    Range errors;
    // In Evaluated::nodes, one per entry of dep_kinds, reduced under use; matches are installed
    // package ids. Empty for a masked candidate.
    std::array<Range, dep_kinds.size()> deps;
    // String ids in Evaluated::ids: REQUIRED_USE's tokens, empty for a masked candidate; and
    // whether its EAPI holds an empty group satisfied.
    Range required_use;
    bool empty_groups_true = false;
    // String ids in Evaluated::ids, one per entry of dep_kinds: the dependency string's tokens,
    // to reduce under other USE (reduce_dependencies). Empty for a masked candidate.
    std::array<Range, dep_kinds.size()> tokens;
    // Its own layers of the USE ledger: its keywords accepted as stable ones (bringing in the
    // *.stable* files), and string ids in Evaluated::ids, the pkginternal layer's USE (IUSE
    // defaults, -test where RESTRICT drops the feature) and the features layer's (test).
    bool stable = false;
    Range internal;
    Range features;
    // String id: its EAPI; and whether that has IUSE_EFFECTIVE, which decides its implicit IUSE.
    std::uint32_t eapi = 0;
    bool iuse_effective = true;

    [[nodiscard]] bool visible() const { return reasons.count == 0; }
};

// One line of the configuration a flag's state is stacked from.
struct LedgerEntry {
    // String id; empty for the environment.
    std::uint32_t file = 0;
    // 0 where portage's value could not be told apart line by line.
    std::uint32_t line = 0;
    // String id; empty for a global entry.
    std::uint32_t atom = 0;
    // String id: USE, or the USE_EXPAND or USE_EXPAND_UNPREFIXED variable it was set through.
    std::uint32_t var = 0;
    // String ids in Evaluated::ids, as portage stacks them: flag, -flag, -*, prefix_*.
    Range tokens;
};

// The files of a profile node or of a repository's profiles directory, in the store's order.
inline constexpr std::array<std::string_view, 12> ledger_files{
    "make.defaults",     "use.stable",
    "use.force",         "use.stable.force",
    "use.mask",          "use.stable.mask",
    "package.use",       "package.use.stable",
    "package.use.force", "package.use.stable.force",
    "package.use.mask",  "package.use.stable.mask"};

// Ranges in Evaluated::ledger_entries, one per entry of ledger_files.
using LedgerSources = std::array<Range, ledger_files.size()>;

struct LedgerNode {
    // String id: the profile directory.
    std::uint32_t path = 0;
    LedgerSources sources;
};

struct LedgerRepository {
    // String ids: its name, and its masters in Evaluated::ids.
    std::uint32_t name = 0;
    Range masters;
    LedgerSources sources;
};

struct LedgerEnvFile {
    // String id: its name under env/.
    std::uint32_t name = 0;
    // In Evaluated::ledger_entries.
    Range entries;
};

// Every source of a flag's state, as portage stacks a package's USE. Ranges of entries are in
// Evaluated::ledger_entries, of string ids in Evaluated::ids.
struct Ledger {
    Range use_order;
    Range use_expand;
    Range use_expand_unprefixed;
    // String id.
    std::uint32_t arch = 0;
    std::vector<LedgerNode> profiles;
    std::vector<LedgerRepository> repositories;
    Range conf;
    Range package_use;
    Range package_env;
    std::vector<LedgerEnvFile> env_files;
    Range env;
    Range env_d;
    Range features;
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
    // String ids of cps, sorted: every cp with an ebuild in a repository, and the cps evaluated
    // on request beside those the installed packages reach.
    Range repository_cps;
    Range requested;
    // String ids, sorted: USE_EXPAND's variables lowercased, and USE_EXPAND_HIDDEN's.
    Range use_expand;
    Range use_expand_hidden;
    std::vector<LedgerEntry> ledger_entries;
    Ledger ledger;

    [[nodiscard]] std::span<const Possible> possible_in(Range range) const EGRAPH_LIFETIMEBOUND {
        return std::span{possible}.subspan(range.first, range.count);
    }
    [[nodiscard]] std::span<const LedgerEntry> entries_in(Range range) const EGRAPH_LIFETIMEBOUND {
        return std::span{ledger_entries}.subspan(range.first, range.count);
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

// The build start the evaluated store at path records, without decoding the rest.
[[nodiscard]] std::expected<std::uint64_t, StoreError>
evaluated_build_time(const std::filesystem::path& path);

// Beside the installed store: its last extension replaced by .evaluated.egraph.
[[nodiscard]] std::filesystem::path evaluated_store_path(const std::filesystem::path& installed);

} // namespace egraph
