#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__clang__)
#define EGRAPH_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define EGRAPH_LIFETIMEBOUND
#endif

namespace egraph {

inline constexpr std::uint32_t store_format_version = 3;
inline constexpr std::array<std::string_view, 5> dep_kinds{"BDEPEND", "DEPEND", "IDEPEND",
                                                           "PDEPEND", "RDEPEND"};

// A slice of one of Store's shared vectors.
struct Range {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

enum class NodeType : std::uint8_t { atom, any_of, all_of, weak_blocker, strong_blocker };

inline constexpr std::uint32_t no_parent = UINT32_MAX;

struct Node {
    NodeType type = NodeType::atom;
    // Index of the enclosing any-of or all-of node in the same list, or no_parent.
    std::uint32_t parent = no_parent;
    // String id of the atom; 0 (the empty string) for groups.
    std::uint32_t atom = 0;
    // Package ids in Store::ids.
    Range matches;
};

// Two string ids: (key, message) for errors, (category, soname) for provided sonames.
struct StringPair {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
};

struct Require {
    std::uint32_t category = 0;
    std::uint32_t soname = 0;
    // Package ids in Store::ids.
    Range providers;
};

struct Package {
    std::uint32_t cpv = 0;
    std::uint32_t cp = 0;
    std::uint32_t slot = 0;
    std::uint32_t sub_slot = 0;
    std::uint32_t repo = 0;
    std::uint32_t eapi = 0;
    // The EAPI has IUSE_EFFECTIVE (5 and later), which decides how implicit IUSE applies.
    bool iuse_effective = false;
    // String ids in Store::ids.
    Range use;
    Range iuse;
    // In Store::pairs.
    Range errors;
    // In Store::nodes, one per entry of dep_kinds.
    std::array<Range, dep_kinds.size()> deps;
    // In Store::pairs.
    Range provided;
    // In Store::required.
    Range required;
};

struct Root {
    std::uint32_t atom = 0;
    Range matches;
};

struct Meta {
    std::string egraph_version;
    std::string portage_version;
    std::string eroot;
    std::uint64_t build_time_ns = 0;
};

// The profile's implicit IUSE: flags that count as in IUSE without the ebuild listing them.
struct ImplicitIuse {
    // IUSE_EFFECTIVE, for packages whose EAPI has it.
    std::vector<std::string> effective;
    // Earlier EAPIs: flags implied exactly, and prefixes implying every flag that starts with them.
    std::vector<std::string> literals;
    std::vector<std::string> prefixes;
};

enum class InputKind : std::uint8_t { file, directory, symlink, missing };

struct Input {
    std::string path;
    InputKind kind = InputKind::file;
    std::uint64_t mtime_ns = 0;
    std::uint64_t size = 0;
};

// A decoded store. Every id and Range in it was checked against its table by decode().
struct Store {
    Meta meta;
    ImplicitIuse implicit;
    std::vector<Input> inputs;
    // Every string back to back; strings[id] slices it.
    std::string pool;
    std::vector<Range> strings;
    std::vector<Package> packages;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> ids;
    std::vector<StringPair> pairs;
    std::vector<Require> required;
    std::vector<Root> roots;

    [[nodiscard]] std::string_view string(std::uint32_t id) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::uint32_t> ids_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const Node> nodes_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const StringPair> pairs_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const Require> required_in(Range range) const EGRAPH_LIFETIMEBOUND;
};

struct StoreError {
    std::string message;
};

// Rejects anything that does not follow docs/store-format.md exactly.
[[nodiscard]] std::expected<Store, StoreError> decode(std::span<const std::byte> data);

[[nodiscard]] std::expected<std::vector<std::byte>, StoreError>
read_file(const std::filesystem::path& path);

[[nodiscard]] std::expected<Store, StoreError> load(const std::filesystem::path& path);

// ${ROOT}${EPREFIX}/var/cache/egraph/installed.egraph
[[nodiscard]] std::filesystem::path default_store_path(const std::filesystem::path& root,
                                                       const std::filesystem::path& eprefix);

} // namespace egraph
