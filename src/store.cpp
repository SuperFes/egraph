#include "store.hpp"

#include "encoding.hpp"

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

using encoding::read_ids;
using encoding::read_nodes;
using encoding::read_pairs;
using encoding::Reader;
using encoding::size32;

constexpr std::uint32_t section_packages = 4;
constexpr std::uint32_t section_roots = 5;
constexpr std::uint32_t section_profile = 6;
constexpr std::size_t section_count = 6;

std::optional<StoreError> read_meta(std::span<const std::byte> section, Store& store) {
    Reader r(section, "meta");
    store.meta.egraph_version = r.text();
    store.meta.portage_version = r.text();
    store.meta.eroot = r.text();
    store.meta.build_time_ns = r.varint();
    r.finish();
    return r.error();
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
            deps = read_nodes(r, store, strings, count);
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
        root.via = r.index(strings, "string");
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

std::string_view Tables::string(std::uint32_t id) const {
    const auto range = strings.at(id);
    return std::string_view{pool}.substr(range.first, range.count);
}

std::span<const std::uint32_t> Tables::ids_in(Range range) const {
    return std::span{ids}.subspan(range.first, range.count);
}

std::span<const Node> Tables::nodes_in(Range range) const {
    return std::span{nodes}.subspan(range.first, range.count);
}

std::span<const StringPair> Tables::pairs_in(Range range) const {
    return std::span{pairs}.subspan(range.first, range.count);
}

std::span<const Require> Store::required_in(Range range) const {
    return std::span{required}.subspan(range.first, range.count);
}

std::expected<Store, StoreError> decode(std::span<const std::byte> data) {
    const auto found = encoding::sections(data, encoding::installed_magic, "an egraph store",
                                          store_format_version, section_count);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto section = [&found](std::uint32_t id) { return found->at(id - 1); };

    Store store;
    for (const auto& error :
         {read_meta(section(encoding::section_meta), store),
          encoding::read_inputs(section(encoding::section_inputs), store.inputs)}) {
        if (error) {
            return std::unexpected(*error);
        }
    }
    // Packages refer to strings, and roots to both.
    if (auto error = encoding::read_strings(section(encoding::section_strings), store)) {
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
            if (error.mismatch) {
                error.mismatch->path = path;
            }
            return error;
        });
    });
}

std::filesystem::path default_store_path(const std::filesystem::path& root,
                                         const std::filesystem::path& eprefix) {
    return root / eprefix.relative_path() / "var/cache/egraph/installed.egraph";
}

} // namespace egraph
