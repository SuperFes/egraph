#include "store.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace egraph {

namespace {

constexpr std::array<std::byte, 8> magic{std::byte{'E'}, std::byte{'G'}, std::byte{'R'},
                                         std::byte{'A'}, std::byte{'P'}, std::byte{'H'},
                                         std::byte{0},   std::byte{0}};
constexpr std::size_t header_size = 16;
constexpr std::size_t entry_size = 20;
constexpr std::uint32_t section_meta = 1;
constexpr std::uint32_t section_inputs = 2;
constexpr std::uint32_t section_strings = 3;
constexpr std::uint32_t section_packages = 4;
constexpr std::uint32_t section_roots = 5;
constexpr std::uint32_t section_profile = 6;
constexpr std::size_t section_count = 6;
constexpr std::uint32_t node_type_count = 5;
constexpr std::uint32_t input_kind_count = 4;

// decode() rejects files of 4 GiB and more, so every size derived from one fits.
std::uint32_t size32(std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("store size exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(size);
}

std::uint64_t little_endian(std::span<const std::byte> bytes) {
    std::uint64_t value = 0;
    for (const auto byte : std::views::reverse(bytes)) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

// Reads one section. The first failure sticks: later reads return zero, which ends every loop,
// and error() reports where it happened.
class Reader {
  public:
    Reader(std::span<const std::byte> data, std::string_view name) : data_(data), name_(name) {}

    std::uint64_t varint() {
        if (!ok()) {
            return 0;
        }
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (pos_ >= data_.size()) {
                fail("truncated varint");
                return 0;
            }
            const auto byte = std::to_integer<std::uint8_t>(data_.subspan(pos_).front());
            ++pos_;
            if (shift == 63 && byte > 1) {
                fail("varint exceeds 64 bits");
                return 0;
            }
            value |= std::uint64_t{byte & 0x7FU} << shift;
            if ((byte & 0x80U) == 0) {
                return value;
            }
        }
        fail("varint exceeds 64 bits");
        return 0;
    }

    std::uint32_t u32() {
        const auto value = varint();
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            fail("value exceeds 32 bits");
            return 0;
        }
        return static_cast<std::uint32_t>(value);
    }

    // An id into a table of limit entries.
    std::uint32_t index(std::uint32_t limit, std::string_view what) {
        const auto value = u32();
        if (ok() && value >= limit) {
            fail_range(what, value, limit);
            return 0;
        }
        return value;
    }

    // Every element takes at least a byte, so a count beyond the bytes left is corrupt; checking
    // it here bounds every allocation by the file size.
    std::uint32_t count() {
        const auto value = u32();
        if (value > data_.size() - pos_) {
            fail_count(value);
            return 0;
        }
        return value;
    }

    std::span<const std::byte> bytes() {
        const auto size = count();
        const auto out = data_.subspan(pos_, size);
        pos_ += size;
        return out;
    }

    std::string text() {
        std::string out;
        for (const auto byte : bytes()) {
            out.push_back(std::to_integer<char>(byte));
        }
        return out;
    }

    void finish() {
        if (ok() && pos_ != data_.size()) {
            fail("trailing bytes");
        }
    }

    // Failures are rare and formatting is heavy, so keeping them out of line lets the reads
    // above inline.
    [[gnu::cold]] [[gnu::noinline]] void fail(std::string_view what) {
        if (!error_) {
            error_ = StoreError{std::format("{}: {} at byte {}", name_, what, pos_)};
        }
    }

    [[gnu::cold]] [[gnu::noinline]] void fail_range(std::string_view what, std::uint32_t value,
                                                    std::uint32_t limit) {
        fail(std::format("{} {} out of range {}", what, value, limit));
    }

    [[gnu::cold]] [[gnu::noinline]] void fail_count(std::uint32_t value) {
        fail(std::format("count {} exceeds the bytes left", value));
    }

    [[nodiscard]] bool ok() const { return !error_.has_value(); }
    [[nodiscard]] const std::optional<StoreError>& error() const { return error_; }

  private:
    std::span<const std::byte> data_;
    std::string_view name_;
    std::size_t pos_ = 0;
    std::optional<StoreError> error_;
};

Range read_ids(Reader& r, std::vector<std::uint32_t>& ids, std::uint32_t limit,
               std::string_view what) {
    const Range range{.first = size32(ids.size()), .count = r.count()};
    for (std::uint32_t i = 0; i < range.count && r.ok(); ++i) {
        ids.push_back(r.index(limit, what));
    }
    return range;
}

Range read_pairs(Reader& r, std::vector<StringPair>& pairs, std::uint32_t strings) {
    const Range range{.first = size32(pairs.size()), .count = r.count()};
    for (std::uint32_t i = 0; i < range.count && r.ok(); ++i) {
        const auto first = r.index(strings, "string");
        pairs.push_back({.first = first, .second = r.index(strings, "string")});
    }
    return range;
}

std::optional<StoreError> read_meta(std::span<const std::byte> section, Store& store) {
    Reader r(section, "meta");
    store.meta.egraph_version = r.text();
    store.meta.portage_version = r.text();
    store.meta.eroot = r.text();
    store.meta.build_time_ns = r.varint();
    r.finish();
    return r.error();
}

std::optional<StoreError> read_inputs(std::span<const std::byte> section, Store& store) {
    Reader r(section, "inputs");
    const auto count = r.count();
    store.inputs.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Input input;
        input.path = r.text();
        input.kind = static_cast<InputKind>(r.index(input_kind_count, "input kind"));
        input.mtime_ns = r.varint();
        input.size = r.varint();
        store.inputs.push_back(std::move(input));
    }
    r.finish();
    return r.error();
}

std::optional<StoreError> read_strings(std::span<const std::byte> section, Store& store) {
    Reader r(section, "strings");
    const auto count = r.count();
    store.strings.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        const auto bytes = r.bytes();
        store.strings.push_back(
            {.first = size32(store.pool.size()), .count = size32(bytes.size())});
        for (const auto byte : bytes) {
            store.pool.push_back(std::to_integer<char>(byte));
        }
    }
    if (r.ok() && (store.strings.empty() || store.strings.front().count != 0)) {
        r.fail("string 0 must be empty");
    }
    r.finish();
    return r.error();
}

bool is_group(NodeType type) {
    return type == NodeType::any_of || type == NodeType::all_of;
}

void read_nodes(Reader& r, Store& store, Range& range, std::uint32_t strings,
                std::uint32_t packages) {
    range = {.first = size32(store.nodes.size()), .count = r.count()};
    for (std::uint32_t i = 0; i < range.count && r.ok(); ++i) {
        Node node;
        node.type = static_cast<NodeType>(r.index(node_type_count, "node type"));
        // 0 for top level, otherwise 1 + an earlier index in this list.
        const auto parent = r.index(i + 1, "parent");
        if (parent != 0) {
            node.parent = parent - 1;
            if (!is_group(store.nodes.at(range.first + node.parent).type)) {
                r.fail("parent is not a group");
            }
        }
        node.atom = r.index(strings, "string");
        node.matches = read_ids(r, store.ids, packages, "package");
        if (is_group(node.type) != (node.atom == 0)) {
            r.fail("only groups have no atom");
        }
        if (is_group(node.type) && node.matches.count != 0) {
            r.fail("a group has matches");
        }
        store.nodes.push_back(node);
    }
}

std::optional<StoreError> read_packages(std::span<const std::byte> section, Store& store) {
    Reader r(section, "packages");
    const auto count = r.count();
    const auto strings = size32(store.strings.size());
    store.packages.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Package pkg;
        for (auto* field : {&pkg.cpv, &pkg.cp, &pkg.slot, &pkg.sub_slot, &pkg.repo, &pkg.eapi}) {
            *field = r.index(strings, "string");
        }
        pkg.iuse_effective = r.index(2, "IUSE_EFFECTIVE flag") == 1;
        pkg.use = read_ids(r, store.ids, strings, "string");
        pkg.iuse = read_ids(r, store.ids, strings, "string");
        pkg.errors = read_pairs(r, store.pairs, strings);
        for (auto& deps : pkg.deps) {
            read_nodes(r, store, deps, strings, count);
        }
        pkg.provided = read_pairs(r, store.pairs, strings);
        pkg.required = {.first = size32(store.required.size()), .count = r.count()};
        for (std::uint32_t k = 0; k < pkg.required.count && r.ok(); ++k) {
            Require require;
            require.category = r.index(strings, "string");
            require.soname = r.index(strings, "string");
            require.providers = read_ids(r, store.ids, count, "package");
            store.required.push_back(require);
        }
        store.packages.push_back(pkg);
    }
    r.finish();
    return r.error();
}

std::optional<StoreError> read_profile(std::span<const std::byte> section, Store& store) {
    Reader r(section, "profile");
    for (auto* flags :
         {&store.implicit.effective, &store.implicit.literals, &store.implicit.prefixes}) {
        const auto count = r.count();
        for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
            flags->push_back(r.text());
        }
    }
    r.finish();
    return r.error();
}

std::optional<StoreError> read_roots(std::span<const std::byte> section, Store& store) {
    Reader r(section, "roots");
    const auto count = r.count();
    const auto strings = size32(store.strings.size());
    const auto packages = size32(store.packages.size());
    store.roots.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Root root;
        root.set = r.index(strings, "string");
        root.atom = r.index(strings, "string");
        root.matches = read_ids(r, store.ids, packages, "package");
        store.roots.push_back(root);
    }
    r.finish();
    return r.error();
}

std::unexpected<StoreError> failure(std::string message) {
    return std::unexpected(StoreError{std::move(message)});
}

} // namespace

std::string_view Store::string(std::uint32_t id) const {
    const auto range = strings.at(id);
    return std::string_view{pool}.substr(range.first, range.count);
}

std::span<const std::uint32_t> Store::ids_in(Range range) const {
    return std::span{ids}.subspan(range.first, range.count);
}

std::span<const Node> Store::nodes_in(Range range) const {
    return std::span{nodes}.subspan(range.first, range.count);
}

std::span<const StringPair> Store::pairs_in(Range range) const {
    return std::span{pairs}.subspan(range.first, range.count);
}

std::span<const Require> Store::required_in(Range range) const {
    return std::span{required}.subspan(range.first, range.count);
}

std::expected<Store, StoreError> decode(std::span<const std::byte> data) {
    if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return failure("store of 4 GiB or more");
    }
    if (data.size() < header_size) {
        return failure("truncated header");
    }
    if (!std::ranges::equal(data.first(magic.size()), magic)) {
        return failure("not an egraph store");
    }
    const auto version = little_endian(data.subspan(8, 4));
    if (version != store_format_version) {
        return failure(
            std::format("format version {}, expected {}", version, store_format_version));
    }
    const auto count = little_endian(data.subspan(12, 4));
    if (count != section_count || data.size() < header_size + (entry_size * section_count)) {
        return failure("bad section table");
    }

    std::array<std::optional<std::span<const std::byte>>, section_count> sections;
    for (std::size_t i = 0; i < section_count; ++i) {
        const auto entry = data.subspan(header_size + (entry_size * i), entry_size);
        const auto id = little_endian(entry.first(4));
        const auto offset = little_endian(entry.subspan(4, 8));
        const auto length = little_endian(entry.subspan(12, 8));
        if (id < 1 || id > section_count || sections.at(id - 1).has_value()) {
            return failure(std::format("unexpected section {}", id));
        }
        if (offset > data.size() || length > data.size() - offset) {
            return failure(std::format("section {} outside the file", id));
        }
        sections.at(id - 1) = data.subspan(offset, length);
    }
    const auto section = [&sections](std::uint32_t id) { return *sections.at(id - 1); };

    Store store;
    for (const auto& error :
         {read_meta(section(section_meta), store), read_inputs(section(section_inputs), store)}) {
        if (error) {
            return std::unexpected(*error);
        }
    }
    // Packages refer to strings, and roots to both.
    if (auto error = read_strings(section(section_strings), store)) {
        return std::unexpected(*error);
    }
    if (auto error = read_packages(section(section_packages), store)) {
        return std::unexpected(*error);
    }
    if (auto error = read_roots(section(section_roots), store)) {
        return std::unexpected(*error);
    }
    if (auto error = read_profile(section(section_profile), store)) {
        return std::unexpected(*error);
    }
    return store;
}

std::expected<std::vector<std::byte>, StoreError> read_file(const std::filesystem::path& path) {
    // Size the read from the open file, which rename() cannot swap underneath us.
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        const std::error_code error(errno, std::generic_category());
        return failure(std::format("{}: {}", path.string(), error.message()));
    }
    const auto end = in.tellg();
    if (end < 0) {
        return failure(std::format("{}: cannot determine size", path.string()));
    }
    std::string buffer(static_cast<std::size_t>(end), '\0');
    in.seekg(0);
    if (!in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
        return failure(std::format("{}: short read", path.string()));
    }
    const auto bytes = std::as_bytes(std::span{buffer});
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

std::expected<Store, StoreError> load(const std::filesystem::path& path) {
    return read_file(path).and_then([&path](const std::vector<std::byte>& bytes) {
        return decode(bytes).transform_error([&path](StoreError error) {
            error.message = std::format("{}: {}", path.string(), error.message);
            return error;
        });
    });
}

std::filesystem::path default_store_path(const std::filesystem::path& root,
                                         const std::filesystem::path& eprefix) {
    return root / eprefix.relative_path() / "var/cache/egraph/installed.egraph";
}

} // namespace egraph
