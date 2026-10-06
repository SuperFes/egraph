#pragma once

// The repository index (docs/store-format.md): every version in the repositories, and what
// portage decides their visibility from, as it parsed it.

#include "store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace egraph {

inline constexpr std::uint32_t repository_format_version = 1;

struct RepositoryMeta {
    std::string egraph_version;
    std::string portage_version;
    std::string eroot;
    std::uint64_t build_time_ns = 0;
};

struct Repository {
    std::uint32_t name = 0;
    std::uint32_t location = 0;
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
    // The flags enabled that LICENSE's or PROPERTIES's conditionals test, where they have any.
    Range use;
    std::uint32_t description = 0;
    std::uint32_t homepage = 0;
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
struct Visibility {
    std::vector<Eapi> eapis;
    Range accept_keywords;
    Range environment_keywords;
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

// A decoded repository index. Every id and Range in it was checked against its table.
struct RepositoryIndex : Tables {
    RepositoryMeta meta;
    std::vector<Input> inputs;
    std::vector<Repository> repositories;
    std::vector<IndexVersion> versions;
    std::vector<ConfigEntry> entries;
    Visibility visibility;

    [[nodiscard]] std::span<const ConfigEntry> entries_in(Range range) const EGRAPH_LIFETIMEBOUND;
};

// Rejects anything that does not follow docs/store-format.md exactly.
[[nodiscard]] std::expected<RepositoryIndex, StoreError>
decode_repository(std::span<const std::byte> data);

[[nodiscard]] std::expected<RepositoryIndex, StoreError>
load_repository(const std::filesystem::path& path);

// Beside the installed store: its last extension replaced by .repository.egraph.
[[nodiscard]] std::filesystem::path repository_index_path(const std::filesystem::path& installed);

} // namespace egraph
