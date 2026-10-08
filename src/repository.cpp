#include "repository.hpp"

#include "encoding.hpp"

#include <format>
#include <limits>
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
constexpr std::uint32_t section_advisories = 7;
constexpr std::uint32_t section_visibility_ledger = 8;
constexpr std::size_t section_count = 8;

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
        Repository repository{.name = r.index(strings, "string")};
        repository.location = r.index(strings, "string");
        repository.description_index = r.index(2, "description index") == 1;
        index.repositories.push_back(repository);
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
        version.invalid = read_ids(r, index.ids, strings, "string");
        index.versions.push_back(version);
    }
    r.finish();
    return r.error();
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
    vis.arch = r.index(strings, "string");
    r.finish();
    return r.error();
}

std::optional<StoreError> read_advisories(std::span<const std::byte> section,
                                          RepositoryIndex& index) {
    Reader r(section, "advisories");
    const auto count = r.count();
    const auto strings = size32(index.strings.size());
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Advisory advisory;
        advisory.id = r.index(strings, "string");
        advisory.title = r.index(strings, "string");
        advisory.synopsis = r.index(strings, "string");
        advisory.revision = r.varint();
        const auto packages = r.count();
        const auto first = size32(index.advisory_packages.size());
        for (std::uint32_t j = 0; j < packages && r.ok(); ++j) {
            AdvisoryPackage package;
            package.cp = r.index(strings, "string");
            package.arch = r.index(strings, "string");
            package.vulnerable = read_ids(r, index.ids, strings, "string");
            package.unaffected = read_ids(r, index.ids, strings, "string");
            index.advisory_packages.push_back(package);
        }
        advisory.packages = {.first = first,
                             .count = size32(index.advisory_packages.size()) - first};
        index.advisories.push_back(advisory);
    }
    r.finish();
    return r.error();
}

Range read_ledger_entries(Reader& r, RepositoryIndex& index, std::uint32_t strings) {
    const auto count = r.count();
    const auto first = size32(index.ledger_entries.size());
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        LedgerEntry entry;
        entry.file = r.index(strings, "string");
        entry.line = r.index(std::numeric_limits<std::uint32_t>::max(), "line");
        entry.atom = r.index(strings, "string");
        entry.var = r.index(strings, "string");
        entry.tokens = read_ids(r, index.ids, strings, "string");
        index.ledger_entries.push_back(entry);
    }
    return {.first = first, .count = size32(index.ledger_entries.size()) - first};
}

std::optional<StoreError> read_visibility_ledger(std::span<const std::byte> section,
                                                 RepositoryIndex& index) {
    Reader r(section, "visibility ledger");
    const auto strings = size32(index.strings.size());
    auto& ledger = index.ledger;
    ledger.env_d = read_ledger_entries(r, index, strings);
    ledger.globals = read_ledger_entries(r, index, strings);
    const auto nodes = r.count();
    for (std::uint32_t i = 0; i < nodes && r.ok(); ++i) {
        VisibilityNode node;
        node.path = r.index(strings, "string");
        for (auto* range :
             {&node.defaults, &node.package_mask, &node.package_unmask, &node.package_keywords,
              &node.package_accept_keywords, &node.package_license}) {
            *range = read_ledger_entries(r, index, strings);
        }
        ledger.profiles.push_back(node);
    }
    const auto repositories = r.count();
    for (std::uint32_t i = 0; i < repositories && r.ok(); ++i) {
        MaskRepository repo;
        repo.name = r.index(strings, "string");
        repo.masters = read_ids(r, index.ids, strings, "string");
        repo.package_mask = read_ledger_entries(r, index, strings);
        repo.package_unmask = read_ledger_entries(r, index, strings);
        ledger.repositories.push_back(repo);
    }
    for (auto* range :
         {&ledger.conf, &ledger.env, &ledger.license_groups, &ledger.package_mask,
          &ledger.package_unmask, &ledger.package_keywords, &ledger.package_accept_keywords,
          &ledger.package_license, &ledger.package_properties, &ledger.package_accept_restrict}) {
        *range = read_ledger_entries(r, index, strings);
    }
    r.finish();
    return r.error();
}

} // namespace

std::span<const LedgerEntry> RepositoryIndex::ledger_entries_in(Range range) const {
    return std::span{ledger_entries}.subspan(range.first, range.count);
}

std::span<const AdvisoryPackage> RepositoryIndex::packages_in(Range range) const {
    return std::span{advisory_packages}.subspan(range.first, range.count);
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
                              read_visibility(section(section_visibility), index),
                              read_advisories(section(section_advisories), index),
                              read_visibility_ledger(section(section_visibility_ledger), index)}) {
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

std::expected<std::uint64_t, StoreError> repository_build_time(const std::filesystem::path& path) {
    return encoding::build_time(path, encoding::repository_magic, "an egraph repository index",
                                repository_format_version, section_count);
}

std::filesystem::path repository_index_path(const std::filesystem::path& installed) {
    auto path = installed;
    return path.replace_extension(".repository.egraph");
}

} // namespace egraph
