#include "repository.hpp"

#include "encoding.hpp"

#include <format>
#include <optional>
#include <utility>

namespace egraph {

namespace {

using encoding::read_ids;
using encoding::Reader;
using encoding::size32;

constexpr std::uint32_t section_repositories = 4;
constexpr std::uint32_t section_versions = 5;
constexpr std::uint32_t section_visibility = 6;
constexpr std::size_t section_count = 6;

std::optional<StoreError> read_meta(std::span<const std::byte> section, RepositoryIndex& index) {
    Reader r(section, "meta");
    index.meta.egraph_version = r.text();
    index.meta.portage_version = r.text();
    index.meta.eroot = r.text();
    index.meta.build_time_ns = r.varint();
    r.finish();
    return r.error();
}

std::optional<StoreError> read_repositories(std::span<const std::byte> section,
                                            RepositoryIndex& index) {
    Reader r(section, "repositories");
    const auto count = r.count();
    const auto strings = size32(index.strings.size());
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        const auto name = r.index(strings, "string");
        index.repositories.push_back({.name = name, .location = r.index(strings, "string")});
    }
    r.finish();
    return r.error();
}

std::optional<StoreError> read_versions(std::span<const std::byte> section,
                                        RepositoryIndex& index) {
    Reader r(section, "versions");
    const auto count = r.count();
    const auto strings = size32(index.strings.size());
    const auto repositories = size32(index.repositories.size());
    index.versions.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        IndexVersion version;
        for (auto* field :
             {&version.cp, &version.cpv, &version.slot, &version.sub_slot, &version.eapi}) {
            *field = r.index(strings, "string");
        }
        version.repository = r.index(repositories, "repository");
        for (auto* tokens : {&version.keywords, &version.license, &version.properties,
                             &version.restrict, &version.use}) {
            *tokens = read_ids(r, index.ids, strings, "string");
        }
        version.description = r.index(strings, "string");
        version.homepage = r.index(strings, "string");
        index.versions.push_back(version);
    }
    r.finish();
    return r.error();
}

Range read_entries(Reader& r, RepositoryIndex& index, std::uint32_t strings) {
    const auto count = r.count();
    const auto first = size32(index.entries.size());
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        const auto atom = r.index(strings, "string");
        index.entries.push_back(
            {.atom = atom, .tokens = read_ids(r, index.ids, strings, "string")});
    }
    return {.first = first, .count = size32(index.entries.size()) - first};
}

std::optional<StoreError> read_visibility(std::span<const std::byte> section,
                                          RepositoryIndex& index) {
    Reader r(section, "visibility");
    const auto strings = size32(index.strings.size());
    auto& vis = index.visibility;
    const auto eapis = r.count();
    for (std::uint32_t i = 0; i < eapis && r.ok(); ++i) {
        Eapi eapi{.eapi = r.index(strings, "string")};
        eapi.supported = r.index(2, "supported") == 1;
        eapi.deprecated = r.index(2, "deprecated") == 1;
        vis.eapis.push_back(eapi);
    }
    vis.accept_keywords = read_ids(r, index.ids, strings, "string");
    vis.environment_keywords = read_ids(r, index.ids, strings, "string");
    for (auto* layers : {&vis.profile_keywords, &vis.profile_accept_keywords}) {
        const auto count = r.count();
        for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
            layers->push_back(read_entries(r, index, strings));
        }
    }
    vis.accept_keywords_entries = read_entries(r, index, strings);
    vis.masks = read_ids(r, index.ids, strings, "string");
    vis.unmasks = read_ids(r, index.ids, strings, "string");
    for (auto [accepted, entries] : {std::pair{&vis.accept_license, &vis.licenses},
                                     std::pair{&vis.accept_properties, &vis.properties},
                                     std::pair{&vis.accept_restrict, &vis.restrict}}) {
        *accepted = read_ids(r, index.ids, strings, "string");
        *entries = read_entries(r, index, strings);
    }
    r.finish();
    return r.error();
}

} // namespace

std::span<const ConfigEntry> RepositoryIndex::entries_in(Range range) const {
    return std::span{entries}.subspan(range.first, range.count);
}

std::expected<RepositoryIndex, StoreError> decode_repository(std::span<const std::byte> data) {
    const auto found =
        encoding::sections(data, encoding::repository_magic, "an egraph repository index",
                           repository_format_version, section_count);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto section = [&found](std::uint32_t id) { return found->at(id - 1); };

    RepositoryIndex index;
    // Records refer to strings, and versions to repositories, so those come first.
    for (const auto& error :
         {read_meta(section(encoding::section_meta), index),
          encoding::read_inputs(section(encoding::section_inputs), index.inputs),
          encoding::read_strings(section(encoding::section_strings), index)}) {
        if (error) {
            return std::unexpected(*error);
        }
    }
    for (const auto& error : {read_repositories(section(section_repositories), index),
                              read_versions(section(section_versions), index),
                              read_visibility(section(section_visibility), index)}) {
        if (error) {
            return std::unexpected(*error);
        }
    }
    return index;
}

std::expected<RepositoryIndex, StoreError> load_repository(const std::filesystem::path& path) {
    return read_file(path).and_then([&path](const std::vector<std::byte>& bytes) {
        return decode_repository(bytes).transform_error([&path](StoreError error) {
            error.message = std::format("{}: {}", path.string(), error.message);
            if (error.mismatch) {
                error.mismatch->path = path;
            }
            return error;
        });
    });
}

std::filesystem::path repository_index_path(const std::filesystem::path& installed) {
    auto path = installed;
    return path.replace_extension(".repository.egraph");
}

} // namespace egraph
