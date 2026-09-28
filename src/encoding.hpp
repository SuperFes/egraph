#pragma once

// The framing both store files share (docs/store-format.md): the header, the section table,
// varints, and the tables every section refers into.

#include "store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph::encoding {

using Magic = std::array<std::byte, 8>;

inline constexpr Magic installed_magic{std::byte{'E'}, std::byte{'G'}, std::byte{'R'},
                                       std::byte{'A'}, std::byte{'P'}, std::byte{'H'},
                                       std::byte{0},   std::byte{0}};
inline constexpr Magic evaluated_magic{std::byte{'E'}, std::byte{'G'}, std::byte{'R'},
                                       std::byte{'A'}, std::byte{'P'}, std::byte{'H'},
                                       std::byte{'E'}, std::byte{'V'}};

inline constexpr std::uint32_t section_meta = 1;
inline constexpr std::uint32_t section_inputs = 2;
inline constexpr std::uint32_t section_strings = 3;

// decode() rejects files of 4 GiB and more, so every size derived from one fits.
[[nodiscard]] std::uint32_t size32(std::size_t size);

// Section i + 1 at index i, after checking the header against magic, version and count.
[[nodiscard]] std::expected<std::vector<std::span<const std::byte>>, StoreError>
sections(std::span<const std::byte> data EGRAPH_LIFETIMEBOUND, const Magic& magic,
         std::string_view kind, std::uint32_t version, std::size_t count);

// Reads one section. The first failure sticks: later reads return zero, which ends every loop,
// and error() reports where it happened.
class Reader {
  public:
    Reader(std::span<const std::byte> data, std::string_view name) : data_(data), name_(name) {}

    std::uint64_t varint();
    std::uint32_t u32();
    // An id into a table of limit entries.
    std::uint32_t index(std::uint32_t limit, std::string_view what);
    // Every element takes at least a byte, so a count beyond the bytes left is corrupt; checking
    // it here bounds every allocation by the file size.
    std::uint32_t count();
    std::span<const std::byte> bytes();
    std::string text();
    void finish();

    // Failures are rare and formatting is heavy, so keeping them out of line lets the reads
    // above inline.
    [[gnu::cold]] [[gnu::noinline]] void fail(std::string_view what);
    [[gnu::cold]] [[gnu::noinline]] void fail_range(std::string_view what, std::uint32_t value,
                                                    std::uint32_t limit);
    [[gnu::cold]] [[gnu::noinline]] void fail_count(std::uint32_t value);

    [[nodiscard]] bool ok() const { return !error_.has_value(); }
    [[nodiscard]] const std::optional<StoreError>& error() const { return error_; }

  private:
    std::span<const std::byte> data_;
    std::string_view name_;
    std::size_t pos_ = 0;
    std::optional<StoreError> error_;
};

Range read_ids(Reader& r, std::vector<std::uint32_t>& ids, std::uint32_t limit,
               std::string_view what);
Range read_pairs(Reader& r, std::vector<StringPair>& pairs, std::uint32_t strings);
// One node list; matches index a table of packages entries.
Range read_nodes(Reader& r, Tables& tables, std::uint32_t strings, std::uint32_t packages);

std::optional<StoreError> read_inputs(std::span<const std::byte> section,
                                      std::vector<Input>& inputs);
std::optional<StoreError> read_strings(std::span<const std::byte> section, Tables& tables);

} // namespace egraph::encoding
