#pragma once

#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace egraph {

inline constexpr std::uint32_t evaluated_format_version = 1;

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
    std::vector<Candidate> candidates;
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

// Beside the installed store: its last extension replaced by .evaluated.egraph.
[[nodiscard]] std::filesystem::path evaluated_store_path(const std::filesystem::path& installed);

} // namespace egraph
