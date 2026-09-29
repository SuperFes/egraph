#include "evaluated.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace egraph {

namespace {

using encoding::read_ids;
using encoding::read_nodes;
using encoding::read_pairs;
using encoding::Reader;
using encoding::size32;

constexpr std::uint32_t section_dependencies = 4;
constexpr std::uint32_t section_candidates = 5;
constexpr std::size_t section_count = 5;
constexpr std::uint32_t source_count = 3;

std::optional<StoreError> read_meta(std::span<const std::byte> section, Evaluated& evaluated) {
    Reader r(section, "meta");
    evaluated.meta.egraph_version = r.text();
    evaluated.meta.portage_version = r.text();
    evaluated.meta.eroot = r.text();
    evaluated.meta.build_time_ns = r.varint();
    evaluated.meta.installed_build_time_ns = r.varint();
    r.finish();
    return r.error();
}

Range read_possible(Reader& r, Evaluated& evaluated, std::uint32_t strings,
                    std::uint32_t packages) {
    const auto count = r.count();
    const auto first = size32(evaluated.possible.size());
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Possible entry;
        entry.kind = r.index(size32(dep_kinds.size()), "kind");
        entry.atom = r.index(strings, "string");
        entry.choice = r.index(2, "choice") == 1;
        entry.matches = read_ids(r, evaluated.ids, packages, "package");
        entry.flags = read_ids(r, evaluated.ids, strings, "string");
        evaluated.possible.push_back(entry);
    }
    return {.first = first, .count = size32(evaluated.possible.size()) - first};
}

std::optional<StoreError> read_dependencies(std::span<const std::byte> section,
                                            Evaluated& evaluated) {
    Reader r(section, "dependencies");
    const auto count = r.count();
    const auto strings = size32(evaluated.strings.size());
    const auto candidates = size32(evaluated.candidates.size());
    evaluated.packages.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Dependencies pkg;
        pkg.cpv = r.index(strings, "string");
        pkg.source = static_cast<DepSource>(r.index(source_count, "source"));
        pkg.eapi = r.index(strings, "string");
        pkg.errors = read_pairs(r, evaluated.pairs, strings);
        for (auto& deps : pkg.deps) {
            deps = read_nodes(r, evaluated, strings, count);
        }
        pkg.possible = read_possible(r, evaluated, strings, count);
        pkg.visible = r.index(2, "visible") == 1;
        pkg.masked = r.index(2, "masked") == 1;
        pkg.vdb_masked = r.index(2, "masked") == 1;
        if (const auto target = r.index(candidates + 1, "candidate"); target != 0) {
            pkg.target = target - 1;
        }
        pkg.rebuild = read_ids(r, evaluated.ids, strings, "string");
        evaluated.packages.push_back(pkg);
    }
    r.finish();
    return r.error();
}

std::optional<StoreError> read_candidates(std::span<const std::byte> section, Evaluated& evaluated,
                                          std::uint32_t packages) {
    Reader r(section, "candidates");
    const auto count = r.count();
    const auto strings = size32(evaluated.strings.size());
    evaluated.candidates.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Candidate candidate;
        for (auto* field : {&candidate.cp, &candidate.cpv, &candidate.repo, &candidate.slot,
                            &candidate.sub_slot}) {
            *field = r.index(strings, "string");
        }
        for (auto* flags : {&candidate.use, &candidate.iuse, &candidate.reasons}) {
            *flags = read_ids(r, evaluated.ids, strings, "string");
        }
        candidate.errors = read_pairs(r, evaluated.pairs, strings);
        for (auto& deps : candidate.deps) {
            deps = read_nodes(r, evaluated, strings, packages);
        }
        evaluated.candidates.push_back(candidate);
    }
    r.finish();
    return r.error();
}

} // namespace

std::expected<Evaluated, StoreError> decode_evaluated(std::span<const std::byte> data) {
    const auto found =
        encoding::sections(data, encoding::evaluated_magic, "an evaluated egraph store",
                           evaluated_format_version, section_count);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto section = [&found](std::uint32_t id) { return found->at(id - 1); };

    Evaluated evaluated;
    // Records refer to strings, so those come first.
    for (const auto& error :
         {read_meta(section(encoding::section_meta), evaluated),
          encoding::read_inputs(section(encoding::section_inputs), evaluated.inputs),
          encoding::read_strings(section(encoding::section_strings), evaluated)}) {
        if (error) {
            return std::unexpected(*error);
        }
    }
    // Dependency records name candidates, whose nodes name the packages of the records.
    Reader packages(section(section_dependencies), "dependencies");
    const auto count = packages.count();
    if (auto error = packages.error()) {
        return std::unexpected(*error);
    }
    if (auto error = read_candidates(section(section_candidates), evaluated, count)) {
        return std::unexpected(*error);
    }
    if (auto error = read_dependencies(section(section_dependencies), evaluated)) {
        return std::unexpected(*error);
    }
    return evaluated;
}

std::expected<Evaluated, StoreError> load_evaluated(const std::filesystem::path& path,
                                                    const Store& installed) {
    const auto failed = [&path](std::string message) {
        return std::unexpected(StoreError{std::format("{}: {}", path.string(), message)});
    };
    const auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto evaluated = decode_evaluated(*bytes);
    if (!evaluated) {
        return failed(std::move(evaluated.error().message));
    }
    if (evaluated->packages.size() != installed.packages.size()) {
        return failed(std::format("{} packages, the installed store has {}",
                                  evaluated->packages.size(), installed.packages.size()));
    }
    for (std::size_t i = 0; i < installed.packages.size(); ++i) {
        const auto cpv = evaluated->string(evaluated->packages.at(i).cpv);
        if (cpv != installed.string(installed.packages.at(i).cpv)) {
            return failed(std::format("package {} is {}, not the installed store's {}", i, cpv,
                                      installed.string(installed.packages.at(i).cpv)));
        }
    }
    return evaluated;
}

std::expected<Stores, StoreError> load_stores(const std::filesystem::path& path) {
    auto installed = load(path);
    if (!installed) {
        return std::unexpected(installed.error());
    }
    auto evaluated = load_evaluated(evaluated_store_path(path), *installed);
    if (!evaluated) {
        return std::unexpected(evaluated.error());
    }
    return Stores{.installed = std::move(*installed), .evaluated = std::move(*evaluated)};
}

Store with_dynamic_deps(Store store, const Evaluated& evaluated) {
    // Evaluated strings follow the installed ones; 0 stays the empty string that groups use.
    const auto offset = size32(store.strings.size());
    const auto pool = size32(store.pool.size());
    store.pool += evaluated.pool;
    for (const auto range : evaluated.strings) {
        store.strings.push_back({.first = pool + range.first, .count = range.count});
    }
    const auto string_id = [offset](std::uint32_t id) { return id == 0 ? 0 : offset + id; };
    const auto is_dependency = [&store](std::uint32_t id) {
        return std::ranges::contains(dep_kinds, store.string(id));
    };

    for (std::size_t i = 0; i < store.packages.size(); ++i) {
        auto& pkg = store.packages.at(i);
        const auto& dynamic = evaluated.packages.at(i);

        // Copied out first: appending to pairs moves what pairs_in views.
        const auto installed = store.pairs_in(pkg.errors);
        std::vector<StringPair> kept(installed.begin(), installed.end());
        std::erase_if(kept, [&](const StringPair& pair) { return is_dependency(pair.first); });
        const auto first = size32(store.pairs.size());
        store.pairs.insert(store.pairs.end(), kept.begin(), kept.end());
        for (const auto pair : evaluated.pairs_in(dynamic.errors)) {
            store.pairs.push_back(
                {.first = string_id(pair.first), .second = string_id(pair.second)});
        }
        pkg.errors = {.first = first, .count = size32(store.pairs.size()) - first};

        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            const auto nodes = evaluated.nodes_in(dynamic.deps.at(kind));
            pkg.deps.at(kind) = {.first = size32(store.nodes.size()),
                                 .count = size32(nodes.size())};
            for (auto node : nodes) {
                const auto matches = evaluated.ids_in(node.matches);
                node.atom = string_id(node.atom);
                node.matches = {.first = size32(store.ids.size()), .count = size32(matches.size())};
                store.ids.insert(store.ids.end(), matches.begin(), matches.end());
                store.nodes.push_back(node);
            }
        }
    }
    return store;
}

std::filesystem::path evaluated_store_path(const std::filesystem::path& installed) {
    auto path = installed;
    return path.replace_extension(".evaluated.egraph");
}

} // namespace egraph
