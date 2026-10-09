#pragma once

// The repository index (docs/store-format.md): every version in the repositories, what portage
// decides their visibility from, and the main repository's GLSAs, as it parsed them.

#include "store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace egraph {

inline constexpr std::uint32_t repository_format_version = 8;

struct RepositoryMeta {
    std::string egraph_version;
    std::string portage_version;
    std::string eroot;
    std::uint64_t build_time_ns = 0;
};

struct Repository {
    std::uint32_t name = 0;
    std::uint32_t location = 0;
    // Whether emerge --search reads its descriptions from a metadata/pkg_desc_index.
    bool description_index = false;
};

// The files SRC_URI names in the same places, and their bytes: fetched once when the
// conditionals over any one of those places are all met.
struct FetchGroup {
    // In RepositoryIndex::fetch_places, per place its conditionals: string ids, "flag?" or
    // "!flag?", the outermost first.
    Range places;
    std::uint64_t bytes = 0;
};

// One version of a cp in one repository.
struct IndexVersion {
    std::uint32_t cp = 0;
    std::uint32_t cpv = 0;
    std::uint32_t slot = 0;
    std::uint32_t sub_slot = 0;
    std::uint32_t eapi = 0;
    // Into RepositoryIndex::repositories.
    std::uint32_t repository = 0;
    // String ids, each string's tokens as use_reduce splits it.
    Range keywords;
    Range license;
    Range properties;
    Range restrict;
    // The flags enabled that LICENSE's, PROPERTIES's or RESTRICT's conditionals test, where
    // LICENSE or PROPERTIES has one.
    Range use;
    std::uint32_t description = 0;
    std::uint32_t homepage = 0;
    // String ids: what depgraph finds invalid in it, as it words each after "invalid: ".
    Range invalid;
    // String ids: the eclasses it inherits, as INHERITED lists them.
    Range inherited;
    // In RepositoryIndex::fetch_groups: its distfiles, in SRC_URI's order.
    Range fetch;
};

struct Eapi {
    std::uint32_t eapi = 0;
    bool supported = false;
    bool deprecated = false;
};

// What the visibility ledger leaves to portage itself.
struct VisibilityConfig {
    std::vector<Eapi> eapis;
    // String id.
    std::uint32_t arch = 0;
};

// A profile node's sources of visibility, ranges of RepositoryIndex::ledger_entries.
struct VisibilityNode {
    std::uint32_t path = 0;
    // make.defaults' ACCEPT_ variables.
    Range defaults;
    Range package_mask;
    Range package_unmask;
    Range package_keywords;
    Range package_accept_keywords;
    // Only in the profile-license format.
    Range package_license;
};

struct MaskRepository {
    std::uint32_t name = 0;
    // String ids.
    Range masters;
    Range package_mask;
    Range package_unmask;
};

// Every source of a version's visibility, entry by entry (docs/store-format.md).
struct VisibilityLedger {
    Range env_d;
    Range globals;
    std::vector<VisibilityNode> profiles;
    std::vector<MaskRepository> repositories;
    // make.conf, then the `*/*` lines of package.license, package.properties and
    // package.accept_restrict.
    Range conf;
    Range env;
    // A group per entry: its name as var, its members as tokens.
    Range license_groups;
    Range package_mask;
    Range package_unmask;
    Range package_keywords;
    Range package_accept_keywords;
    Range package_license;
    Range package_properties;
    Range package_accept_restrict;
};

// A package entry of a GLSA.
struct AdvisoryPackage {
    std::uint32_t cp = 0;
    // "*" or the keywords it applies to, space-separated.
    std::uint32_t arch = 0;
    // String ids: atoms as portage's glsa module makes them, the revision ranges (>=~, >~, <=~,
    // <~) included.
    Range vulnerable;
    Range unaffected;
};

struct Advisory {
    std::uint32_t id = 0;
    std::uint32_t title = 0;
    std::uint32_t synopsis = 0;
    // The <revised> count, which a change to it raises.
    std::uint64_t revision = 0;
    // In RepositoryIndex::advisory_packages.
    Range packages;
};

// Where binary packages are listed: PKGDIR, or a binhost portage keeps a copy of the index of.
enum class BinarySourceKind : std::uint8_t { local = 0, binhost = 1 };

struct BinarySource {
    BinarySourceKind kind = BinarySourceKind::local;
    // String ids: the binhost's name in binrepos.conf (empty for PKGDIR), and PKGDIR or its URI.
    std::uint32_t name = 0;
    std::uint32_t location = 0;
};

// What emerge would refuse a binary package for apart from its USE, as bits.
inline constexpr std::uint32_t binary_changed_deps = 1;
inline constexpr std::uint32_t binary_user_patches = 2;

// A binary package one of the binary sources lists.
struct Binary {
    // String ids.
    std::uint32_t cpv = 0;
    std::uint32_t slot = 0;
    // Into RepositoryIndex::binary_sources.
    std::uint32_t source = 0;
    // 0 without one.
    std::uint64_t build_id = 0;
    std::uint64_t size = 0;
    // String ids, sorted: the flags it was built with, and its IUSE without defaults.
    Range use;
    Range iuse;
    // binary_changed_deps and binary_user_patches.
    std::uint32_t ignored = 0;
};

// A decoded repository index. Every id and Range in it was checked against its table.
struct RepositoryIndex : Tables {
    RepositoryMeta meta;
    std::vector<Input> inputs;
    std::vector<Repository> repositories;
    std::vector<IndexVersion> versions;
    std::vector<FetchGroup> fetch_groups;
    std::vector<Range> fetch_places;
    VisibilityConfig visibility;
    // Sorted by id.
    std::vector<Advisory> advisories;
    std::vector<AdvisoryPackage> advisory_packages;
    std::vector<LedgerEntry> ledger_entries;
    VisibilityLedger ledger;
    // PKGDIR first, then the binhosts by descending priority.
    std::vector<BinarySource> binary_sources;
    // Sorted by cpv, then by source, then by build id.
    std::vector<Binary> binaries;

    [[nodiscard]] std::span<const LedgerEntry>
    ledger_entries_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const AdvisoryPackage>
    packages_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const FetchGroup> fetch_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const Range> places_in(Range range) const EGRAPH_LIFETIMEBOUND;
};

// The bytes of the distfiles version fetches under use (sorted): those of the groups with a
// place whose conditionals use meets, as portage's fetch map holds them under it.
[[nodiscard]] std::uint64_t distfiles_size(const RepositoryIndex& index,
                                           const IndexVersion& version,
                                           std::span<const std::string> use);

// Rejects anything that does not follow docs/store-format.md exactly.
[[nodiscard]] std::expected<RepositoryIndex, StoreError>
decode_repository(std::span<const std::byte> data);

[[nodiscard]] std::expected<RepositoryIndex, StoreError>
load_repository(const std::filesystem::path& path);

// The build start the repository index at path records, without decoding the rest.
[[nodiscard]] std::expected<std::uint64_t, StoreError>
repository_build_time(const std::filesystem::path& path);

// Beside the installed store: its last extension replaced by .repository.egraph.
[[nodiscard]] std::filesystem::path repository_index_path(const std::filesystem::path& installed);

} // namespace egraph
