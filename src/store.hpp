#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__clang__)
#define EGRAPH_LIFETIMEBOUND [[clang::lifetimebound]]
// A parameter a member function keeps a reference to.
#define EGRAPH_KEPT_BY_THIS [[clang::lifetime_capture_by_this]]
#else
#define EGRAPH_LIFETIMEBOUND
#define EGRAPH_KEPT_BY_THIS
#endif

namespace egraph {

inline constexpr std::uint32_t store_format_version = 6;
inline constexpr std::array<std::string_view, 5> dep_kinds{"BDEPEND", "DEPEND", "IDEPEND",
                                                           "PDEPEND", "RDEPEND"};

// DEPEND and BDEPEND, which emerge --with-bdeps=n leaves out.
[[nodiscard]] inline bool is_build_kind(std::uint32_t kind) {
    return dep_kinds.at(kind) == "DEPEND" || dep_kinds.at(kind) == "BDEPEND";
}

// A slice of one of Store's shared vectors.
struct Range {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

// One line of the configuration (the evaluated store's USE ledger, the repository index's
// visibility ledger), with the file and line it was read from.
struct LedgerEntry {
    // String id; empty for the environment and portage's built-in defaults.
    std::uint32_t file = 0;
    // 0 where portage's value could not be told apart line by line.
    std::uint32_t line = 0;
    // String id: the line's atom (a mask file's with its `-`); empty for a global entry.
    std::uint32_t atom = 0;
    // String id: the variable it was set through (USE, a USE_EXPAND variable, an ACCEPT_
    // variable), a license group's name, or empty for a package.* line.
    std::uint32_t var = 0;
    // String ids in the store's ids, as portage stacks them.
    Range tokens;
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
    // The vdb entry's COUNTER, and when it was merged in seconds since the epoch; 0 when unknown.
    std::uint64_t counter = 0;
    std::uint64_t merged = 0;
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

// An atom of a root set (selected, system, profile): what depclean keeps regardless.
struct Root {
    // String ids.
    std::uint32_t set = 0;
    std::uint32_t atom = 0;
    // For an atom of @selected, the set world_sets names that holds it, which depclean keeps when
    // its arguments empty @selected; the empty string for any other.
    std::uint32_t via = 0;
    // Package ids in Store::ids.
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
    auto operator<=>(const Input&) const = default;
};

// The shared vectors a store file's records slice with Ranges.
struct Tables {
    // Every string back to back; strings[id] slices it.
    std::string pool;
    std::vector<Range> strings;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> ids;
    std::vector<StringPair> pairs;

    [[nodiscard]] std::string_view string(std::uint32_t id) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::uint32_t> ids_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const Node> nodes_in(Range range) const EGRAPH_LIFETIMEBOUND;
    [[nodiscard]] std::span<const StringPair> pairs_in(Range range) const EGRAPH_LIFETIMEBOUND;
};

// Appends strings to a table, each once; ids already in it stay as they are.
class Interner {
  public:
    explicit Interner(Tables& tables EGRAPH_KEPT_BY_THIS);

    std::uint32_t operator()(const std::string& text);

  private:
    // Never null; the table outlives the interner.
    Tables* tables_;
    std::unordered_map<std::string, std::uint32_t> ids_;
};

// A decoded store. Every id and Range in it was checked against its table by decode().
struct Store : Tables {
    Meta meta;
    ImplicitIuse implicit;
    std::vector<Input> inputs;
    std::vector<Package> packages;
    std::vector<Require> required;
    std::vector<Root> roots;

    [[nodiscard]] std::span<const Require> required_in(Range range) const EGRAPH_LIFETIMEBOUND;
};

// A store in another format version than this egraph reads: written by another version.
struct FormatMismatch {
    // "an egraph store", "an evaluated egraph store".
    std::string kind;
    std::uint32_t found = 0;
    std::uint32_t expected = 0;
    // The file, once loaded from one.
    std::filesystem::path path{};
};

struct StoreError {
    std::string message;
    std::optional<FormatMismatch> mismatch{};
};

// Rejects anything that does not follow docs/store-format.md exactly.
[[nodiscard]] std::expected<Store, StoreError> decode(std::span<const std::byte> data);

[[nodiscard]] std::expected<std::vector<std::byte>, StoreError>
read_file(const std::filesystem::path& path);

[[nodiscard]] std::expected<Store, StoreError> load(const std::filesystem::path& path);

// The build start the store at path records, without decoding the rest.
[[nodiscard]] std::expected<std::uint64_t, StoreError>
store_build_time(const std::filesystem::path& path);

// ${ROOT}${EPREFIX}/var/cache/egraph/installed.egraph
[[nodiscard]] std::filesystem::path default_store_path(const std::filesystem::path& root,
                                                       const std::filesystem::path& eprefix);

} // namespace egraph
