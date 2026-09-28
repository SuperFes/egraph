#include "encoding.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace egraph::encoding {

namespace {

constexpr std::size_t header_size = 16;
constexpr std::size_t entry_size = 20;
constexpr std::uint32_t node_type_count = 5;
constexpr std::uint32_t input_kind_count = 4;

std::uint64_t little_endian(std::span<const std::byte> bytes) {
    std::uint64_t value = 0;
    for (const auto byte : std::views::reverse(bytes)) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

std::unexpected<StoreError> failure(std::string message) {
    return std::unexpected(StoreError{std::move(message)});
}

bool is_group(NodeType type) {
    return type == NodeType::any_of || type == NodeType::all_of;
}

} // namespace

std::uint32_t size32(std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("store size exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(size);
}

std::expected<std::vector<std::span<const std::byte>>, StoreError>
sections(std::span<const std::byte> data, const Magic& magic, std::string_view kind,
         std::uint32_t version, std::size_t count) {
    if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return failure("store of 4 GiB or more");
    }
    if (data.size() < header_size) {
        return failure("truncated header");
    }
    if (!std::ranges::equal(data.first(magic.size()), magic)) {
        return failure(std::format("not {}", kind));
    }
    const auto found_version = little_endian(data.subspan(8, 4));
    if (found_version != version) {
        return failure(std::format("format version {}, expected {}", found_version, version));
    }
    if (little_endian(data.subspan(12, 4)) != count ||
        data.size() < header_size + (entry_size * count)) {
        return failure("bad section table");
    }

    std::vector<std::optional<std::span<const std::byte>>> found(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto entry = data.subspan(header_size + (entry_size * i), entry_size);
        const auto id = little_endian(entry.first(4));
        const auto offset = little_endian(entry.subspan(4, 8));
        const auto length = little_endian(entry.subspan(12, 8));
        if (id < 1 || id > count || found.at(id - 1).has_value()) {
            return failure(std::format("unexpected section {}", id));
        }
        if (offset > data.size() || length > data.size() - offset) {
            return failure(std::format("section {} outside the file", id));
        }
        found.at(id - 1) = data.subspan(offset, length);
    }
    // count distinct ids in 1..count: every slot is filled.
    std::vector<std::span<const std::byte>> out;
    out.reserve(count);
    for (const auto& section : found) {
        out.push_back(section.value_or(std::span<const std::byte>{}));
    }
    return out;
}

std::uint64_t Reader::varint() {
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

std::uint32_t Reader::u32() {
    const auto value = varint();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        fail("value exceeds 32 bits");
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t Reader::index(std::uint32_t limit, std::string_view what) {
    const auto value = u32();
    if (ok() && value >= limit) {
        fail_range(what, value, limit);
        return 0;
    }
    return value;
}

std::uint32_t Reader::count() {
    const auto value = u32();
    if (value > data_.size() - pos_) {
        fail_count(value);
        return 0;
    }
    return value;
}

std::span<const std::byte> Reader::bytes() {
    const auto size = count();
    const auto out = data_.subspan(pos_, size);
    pos_ += size;
    return out;
}

std::string Reader::text() {
    std::string out;
    for (const auto byte : bytes()) {
        out.push_back(std::to_integer<char>(byte));
    }
    return out;
}

void Reader::finish() {
    if (ok() && pos_ != data_.size()) {
        fail("trailing bytes");
    }
}

void Reader::fail(std::string_view what) {
    if (!error_) {
        error_ = StoreError{std::format("{}: {} at byte {}", name_, what, pos_)};
    }
}

void Reader::fail_range(std::string_view what, std::uint32_t value, std::uint32_t limit) {
    fail(std::format("{} {} out of range {}", what, value, limit));
}

void Reader::fail_count(std::uint32_t value) {
    fail(std::format("count {} exceeds the bytes left", value));
}

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

Range read_nodes(Reader& r, Tables& tables, std::uint32_t strings, std::uint32_t packages) {
    const Range range{.first = size32(tables.nodes.size()), .count = r.count()};
    for (std::uint32_t i = 0; i < range.count && r.ok(); ++i) {
        Node node;
        node.type = static_cast<NodeType>(r.index(node_type_count, "node type"));
        // 0 for top level, otherwise 1 + an earlier index in this list.
        const auto parent = r.index(i + 1, "parent");
        if (parent != 0) {
            node.parent = parent - 1;
            if (!is_group(tables.nodes.at(range.first + node.parent).type)) {
                r.fail("parent is not a group");
            }
        }
        node.atom = r.index(strings, "string");
        node.matches = read_ids(r, tables.ids, packages, "package");
        if (is_group(node.type) != (node.atom == 0)) {
            r.fail("only groups have no atom");
        }
        if (is_group(node.type) && node.matches.count != 0) {
            r.fail("a group has matches");
        }
        tables.nodes.push_back(node);
    }
    return range;
}

std::optional<StoreError> read_inputs(std::span<const std::byte> section,
                                      std::vector<Input>& inputs) {
    Reader r(section, "inputs");
    const auto count = r.count();
    inputs.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Input input;
        input.path = r.text();
        input.kind = static_cast<InputKind>(r.index(input_kind_count, "input kind"));
        input.mtime_ns = r.varint();
        input.size = r.varint();
        inputs.push_back(std::move(input));
    }
    r.finish();
    return r.error();
}

std::optional<StoreError> read_strings(std::span<const std::byte> section, Tables& tables) {
    Reader r(section, "strings");
    const auto count = r.count();
    tables.strings.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        const auto bytes = r.bytes();
        tables.strings.push_back(
            {.first = size32(tables.pool.size()), .count = size32(bytes.size())});
        for (const auto byte : bytes) {
            tables.pool.push_back(std::to_integer<char>(byte));
        }
    }
    if (r.ok() && (tables.strings.empty() || tables.strings.front().count != 0)) {
        r.fail("string 0 must be empty");
    }
    r.finish();
    return r.error();
}

} // namespace egraph::encoding
