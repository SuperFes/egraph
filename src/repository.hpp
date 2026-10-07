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

inline constexpr std::uint32_t repository_format_version = 4;

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
};

struct Eapi {
    std::uint32_t eapi = 0;
    bool supported = false;
    bool deprecated = false;
};

// A package.* line: an atom, wildcards allowed, and its tokens.
struct ConfigEntry {
    std::uint32_t atom = 0;
    Range tokens;
};

// String ids and ranges of RepositoryIndex::entries, as docs/store-format.md lists them.
struct VisibilityConfig {
    std::vector<Eapi> eapis;
    Range accept_keywords;
    Range environment_keywords;
    std::uint32_t arch = 0;
    std::vector<Range> profile_keywords;
    std::vector<Range> profile_accept_keywords;
    Range accept_keywords_entries;
    Range masks;
    Range unmasks;
    Range accept_license;
    Range licenses;
    Range accept_properties;
    Range properties;
    Range accept_restrict;
    Range restrict;
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

// A decoded repository index. Every id and Range in it was checked against its table.
struct RepositoryIndex : Tables {
    RepositoryMeta meta;
    std::vector<Input> inputs;
    std::vector<Repository> repositories;
    std::vector<IndexVersion> versions;
    std::vector<ConfigEntry> entries;
    VisibilityConfig visibility;
    // Sorted by id.
    std::vector<Advisory> advisories;
    std::vector<AdvisoryPackage> advisory_packages;

    [[nodiscard]] std::span<const ConfigEntry> entries_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const AdvisoryPackage>
    packages_in(Range range) const EGRAPH_LIFETIMEBOUND;
};

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
