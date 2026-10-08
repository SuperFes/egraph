#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace egraph {

namespace {

struct CodePoint {
    char32_t value = 0;
    std::size_t length = 0;
};

unsigned byte_at(std::string_view bytes, std::size_t index) {
    return static_cast<unsigned char>(bytes.at(index));
}

// One well-formed UTF-8 sequence at the front of bytes, by the table Python's decoder uses
// (Unicode 3-7): no overlongs, no surrogates, nothing past U+10FFFF.
std::optional<CodePoint> decode_utf8(std::string_view bytes) {
    const unsigned lead = byte_at(bytes, 0);
    if (lead < 0x80U) {
        return CodePoint{.value = lead, .length = 1};
    }
    std::size_t length = 0;
    char32_t value = 0;
    unsigned low = 0x80U;
    unsigned high = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
        value = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3;
        value = lead & 0x0FU;
        low = lead == 0xE0U ? 0xA0U : low;
        high = lead == 0xEDU ? 0x9FU : high;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
        value = lead & 0x07U;
        low = lead == 0xF0U ? 0x90U : low;
        high = lead == 0xF4U ? 0x8FU : high;
    } else {
        return std::nullopt;
    }
    if (bytes.size() < length) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned next = byte_at(bytes, i);
        if (next < (i == 1 ? low : 0x80U) || next > (i == 1 ? high : 0xBFU)) {
            return std::nullopt;
        }
        value = (value << 6U) | (next & 0x3FU);
    }
    return CodePoint{.value = value, .length = length};
}

void write_unit(std::ostream& out, char32_t unit) {
    out << std::format("\\u{:04x}", static_cast<std::uint32_t>(unit));
}

void write_code_point(std::ostream& out, char32_t code) {
    switch (code) {
    case U'"':
        out << "\\\"";
        return;
    case U'\\':
        out << "\\\\";
        return;
    case U'\n':
        out << "\\n";
        return;
    case U'\r':
        out << "\\r";
        return;
    case U'\t':
        out << "\\t";
        return;
    case U'\b':
        out << "\\b";
        return;
    case U'\f':
        out << "\\f";
        return;
    default:
        break;
    }
    if (code >= 0x20U && code < 0x7FU) {
        out << static_cast<char>(code);
    } else if (code < 0x10000U) {
        write_unit(out, code);
    } else {
        const char32_t offset = code - 0x10000U;
        write_unit(out, 0xD800U + (offset >> 10U));
        write_unit(out, 0xDC00U + (offset & 0x3FFU));
    }
}

void write_string(std::ostream& out, const Tables& store, std::uint32_t id) {
    write_json_string(out, store.string(id));
}

void write_string_list(std::ostream& out, const Tables& store, Range range) {
    out << '[';
    bool first = true;
    for (const auto id : store.ids_in(range)) {
        out << (first ? "" : ",");
        first = false;
        write_string(out, store, id);
    }
    out << ']';
}

void write_pairs(std::ostream& out, const Tables& store, Range range) {
    out << '[';
    bool first = true;
    for (const auto& pair : store.pairs_in(range)) {
        out << (first ? "[" : ",[");
        first = false;
        write_string(out, store, pair.first);
        out << ',';
        write_string(out, store, pair.second);
        out << ']';
    }
    out << ']';
}

constexpr std::array<std::string_view, 5> node_type_names{"atom", "any-of", "all-of",
                                                          "weak-blocker", "strong-blocker"};

// Matches name the packages of Layer, a Store or an Evaluated, by cpv.
template <typename Layer> void write_nodes(std::ostream& out, const Layer& store, Range range) {
    out << '[';
    bool first = true;
    for (const auto& node : store.nodes_in(range)) {
        out << (first ? "{\"atom\":" : ",{\"atom\":");
        first = false;
        write_string(out, store, node.atom);
        out << ",\"matches\":[";
        bool first_match = true;
        for (const auto id : store.ids_in(node.matches)) {
            out << (first_match ? "" : ",");
            first_match = false;
            write_string(out, store, store.packages.at(id).cpv);
        }
        out << "],\"parent\":";
        if (node.parent == no_parent) {
            out << "-1";
        } else {
            out << node.parent;
        }
        out << R"(,"type":")" << node_type_names.at(static_cast<std::size_t>(node.type)) << R"("})";
    }
    out << ']';
}

// The node lists of a package or candidate, one per dependency kind, as a JSON object.
template <typename Layer>
void write_deps(std::ostream& out, const Layer& store,
                const std::array<Range, dep_kinds.size()>& deps) {
    out << '{';
    for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
        out << (kind == 0 ? "\"" : ",\"") << dep_kinds.at(kind) << "\":";
        write_nodes(out, store, deps.at(kind));
    }
    out << '}';
}

void write_package(std::ostream& out, const Store& store, const Package& pkg) {
    out << "{\"counter\":" << pkg.counter << ",\"cp\":";
    write_string(out, store, pkg.cp);
    out << ",\"cpv\":";
    write_string(out, store, pkg.cpv);
    out << ",\"deps\":";
    write_deps(out, store, pkg.deps);
    out << ",\"eapi\":";
    write_string(out, store, pkg.eapi);
    out << ",\"errors\":";
    write_pairs(out, store, pkg.errors);
    out << ",\"iuse\":";
    write_string_list(out, store, pkg.iuse);
    out << ",\"merged\":" << pkg.merged << ",\"provides\":";
    write_pairs(out, store, pkg.provided);
    out << ",\"repo\":";
    write_string(out, store, pkg.repo);
    out << ",\"requires\":[";
    bool first = true;
    for (const auto& require : store.required_in(pkg.required)) {
        out << (first ? "[" : ",[");
        first = false;
        write_string(out, store, require.category);
        out << ',';
        write_string(out, store, require.soname);
        out << ']';
    }
    out << "],\"slot\":";
    write_string(out, store, pkg.slot);
    out << ",\"sub_slot\":";
    write_string(out, store, pkg.sub_slot);
    out << ",\"use\":";
    write_string_list(out, store, pkg.use);
    out << '}';
}

} // namespace

void write_json_string(std::ostream& out, std::string_view bytes) {
    out << '"';
    while (!bytes.empty()) {
        if (const auto code = decode_utf8(bytes)) {
            write_code_point(out, code->value);
            bytes.remove_prefix(code->length);
        } else {
            // Python's surrogateescape: each undecodable byte becomes U+DC80..U+DCFF.
            write_unit(out, 0xDC00U + byte_at(bytes, 0));
            bytes.remove_prefix(1);
        }
    }
    out << '"';
}

namespace {

void write_ledger_entries(std::ostream& out, const Tables& tables,
                          std::span<const LedgerEntry> entries) {
    out << '[';
    bool first = true;
    for (const auto& entry : entries) {
        out << (first ? "{\"atom\":" : ",{\"atom\":");
        first = false;
        write_string(out, tables, entry.atom);
        out << ",\"file\":";
        write_string(out, tables, entry.file);
        out << ",\"line\":" << entry.line << ",\"tokens\":";
        write_string_list(out, tables, entry.tokens);
        out << ",\"var\":";
        write_string(out, tables, entry.var);
        out << '}';
    }
    out << ']';
}

void write_entries(std::ostream& out, const Evaluated& evaluated, Range range) {
    write_ledger_entries(out, evaluated, evaluated.entries_in(range));
}

// A profile node's or a repository's object: its files beside its other members, every key in
// sorted order as the builder's canonical JSON has them.
template <typename Members>
void write_sources(std::ostream& out, const Evaluated& evaluated, const LedgerSources& sources,
                   std::initializer_list<std::string_view> names, const Members& members) {
    std::vector<std::string_view> keys(ledger_files.begin(), ledger_files.end());
    keys.insert(keys.end(), names);
    std::ranges::sort(keys);
    out << '{';
    bool first = true;
    for (const auto key : keys) {
        out << (first ? "" : ",");
        first = false;
        write_json_string(out, key);
        out << ':';
        if (const auto file = std::ranges::find(ledger_files, key); file != ledger_files.end()) {
            write_entries(out, evaluated,
                          sources.at(static_cast<std::size_t>(file - ledger_files.begin())));
        } else {
            members(key);
        }
    }
    out << '}';
}

void write_ledger(std::ostream& out, const Evaluated& evaluated) {
    const auto& ledger = evaluated.ledger;
    out << R"({"arch":)";
    write_string(out, evaluated, ledger.arch);
    out << ",\"conf\":";
    write_entries(out, evaluated, ledger.conf);
    out << ",\"env\":";
    write_entries(out, evaluated, ledger.env);
    out << ",\"env_d\":";
    write_entries(out, evaluated, ledger.env_d);
    out << ",\"env_files\":[";
    bool first = true;
    for (const auto& file : ledger.env_files) {
        out << (first ? "{\"entries\":" : ",{\"entries\":");
        first = false;
        write_entries(out, evaluated, file.entries);
        out << ",\"name\":";
        write_string(out, evaluated, file.name);
        out << '}';
    }
    out << "],\"features\":";
    write_string_list(out, evaluated, ledger.features);
    out << ",\"package_env\":";
    write_entries(out, evaluated, ledger.package_env);
    out << ",\"package_use\":";
    write_entries(out, evaluated, ledger.package_use);
    out << ",\"profiles\":[";
    first = true;
    for (const auto& node : ledger.profiles) {
        out << (first ? "" : ",");
        first = false;
        write_sources(out, evaluated, node.sources, {"path"},
                      [&](std::string_view) { write_string(out, evaluated, node.path); });
    }
    out << "],\"repositories\":[";
    first = true;
    for (const auto& repo : ledger.repositories) {
        out << (first ? "" : ",");
        first = false;
        write_sources(out, evaluated, repo.sources, {"masters", "name"}, [&](std::string_view key) {
            if (key == "name") {
                write_string(out, evaluated, repo.name);
            } else {
                write_string_list(out, evaluated, repo.masters);
            }
        });
    }
    out << "],\"use_expand\":";
    write_string_list(out, evaluated, ledger.use_expand);
    out << ",\"use_expand_unprefixed\":";
    write_string_list(out, evaluated, ledger.use_expand_unprefixed);
    out << ",\"use_order\":";
    write_string_list(out, evaluated, ledger.use_order);
    out << '}';
}

} // namespace

void write_evaluated_json(std::ostream& out, const Evaluated& evaluated) {
    constexpr std::array<std::string_view, 3> sources{"ebuild", "vdb", "moved"};
    out << R"({"candidates":[)";
    bool first = true;
    for (const auto& candidate : evaluated.candidates) {
        out << (first ? "{\"cp\":" : ",{\"cp\":");
        first = false;
        write_string(out, evaluated, candidate.cp);
        out << ",\"cpv\":";
        write_string(out, evaluated, candidate.cpv);
        out << ",\"deps\":";
        write_deps(out, evaluated, candidate.deps);
        out << ",\"eapi\":";
        write_string(out, evaluated, candidate.eapi);
        out << ",\"empty_groups_true\":" << (candidate.empty_groups_true ? "true" : "false");
        out << ",\"errors\":";
        write_pairs(out, evaluated, candidate.errors);
        out << ",\"features\":";
        write_string_list(out, evaluated, candidate.features);
        out << ",\"forced\":";
        write_string_list(out, evaluated, candidate.forced);
        out << ",\"internal\":";
        write_string_list(out, evaluated, candidate.internal);
        out << ",\"iuse\":";
        write_string_list(out, evaluated, candidate.iuse);
        out << ",\"iuse_effective\":" << (candidate.iuse_effective ? "true" : "false");
        out << ",\"reasons\":";
        write_string_list(out, evaluated, candidate.reasons);
        out << ",\"repo\":";
        write_string(out, evaluated, candidate.repo);
        out << ",\"required_use\":";
        write_string_list(out, evaluated, candidate.required_use);
        out << ",\"slot\":";
        write_string(out, evaluated, candidate.slot);
        out << ",\"stable\":" << (candidate.stable ? "true" : "false");
        out << ",\"sub_slot\":";
        write_string(out, evaluated, candidate.sub_slot);
        out << ",\"tokens\":[";
        for (std::size_t kind = 0; kind < candidate.tokens.size(); ++kind) {
            out << (kind == 0 ? "" : ",");
            write_string_list(out, evaluated, candidate.tokens.at(kind));
        }
        out << "],\"use\":";
        write_string_list(out, evaluated, candidate.use);
        out << '}';
    }
    out << R"(],"format":11,"ledger":)";
    write_ledger(out, evaluated);
    out << R"(,"packages":[)";
    first = true;
    for (const auto& pkg : evaluated.packages) {
        out << (first ? "{\"cpv\":" : ",{\"cpv\":");
        first = false;
        write_string(out, evaluated, pkg.cpv);
        out << ",\"deps\":";
        write_deps(out, evaluated, pkg.deps);
        out << ",\"eapi\":";
        write_string(out, evaluated, pkg.eapi);
        out << ",\"errors\":";
        write_pairs(out, evaluated, pkg.errors);
        out << ",\"hidden\":" << static_cast<int>(pkg.hidden) << ",\"mask_comment\":";
        write_string(out, evaluated, pkg.mask_comment);
        out << ",\"mask_file\":";
        write_string(out, evaluated, pkg.mask_file);
        out << ",\"mask_reasons\":";
        write_string_list(out, evaluated, pkg.mask_reasons);
        out << ",\"masked\":" << (pkg.masked ? "true" : "false") << ",\"possible\":[";
        bool first_possible = true;
        for (const auto& entry : evaluated.possible_in(pkg.possible)) {
            out << (first_possible ? "{\"atom\":" : ",{\"atom\":");
            first_possible = false;
            write_string(out, evaluated, entry.atom);
            out << ",\"choice\":" << (entry.choice ? "true" : "false") << ",\"flags\":";
            write_string_list(out, evaluated, entry.flags);
            out << R"(,"kind":")" << dep_kinds.at(entry.kind) << R"(","matches":[)";
            bool first_match = true;
            for (const auto id : evaluated.ids_in(entry.matches)) {
                out << (first_match ? "" : ",");
                first_match = false;
                write_string(out, evaluated, evaluated.packages.at(id).cpv);
            }
            out << "]}";
        }
        out << "],\"rebuild\":";
        write_string_list(out, evaluated, pkg.rebuild);
        out << R"(,"source":")" << sources.at(static_cast<std::size_t>(pkg.source))
            << R"(","target":)";
        if (const auto index = pkg.target) {
            const auto& target = evaluated.candidates.at(*index);
            out << "{\"cpv\":";
            write_string(out, evaluated, target.cpv);
            out << ",\"repo\":";
            write_string(out, evaluated, target.repo);
            out << '}';
        } else {
            out << "null";
        }
        out << ",\"vdb_hidden\":" << static_cast<int>(pkg.vdb_hidden) << ",\"vdb_mask_reasons\":";
        write_string_list(out, evaluated, pkg.vdb_mask_reasons);
        out << ",\"vdb_masked\":" << (pkg.vdb_masked ? "true" : "false")
            << ",\"visible\":" << (pkg.visible ? "true" : "false") << '}';
    }
    out << "],\"repository_cps\":";
    write_string_list(out, evaluated, evaluated.repository_cps);
    out << ",\"requested\":";
    write_string_list(out, evaluated, evaluated.requested);
    out << ",\"use_expand\":";
    write_string_list(out, evaluated, evaluated.use_expand);
    out << ",\"use_expand_hidden\":";
    write_string_list(out, evaluated, evaluated.use_expand_hidden);
    out << "}\n";
}

namespace {

void write_visibility_ledger(std::ostream& out, const RepositoryIndex& index) {
    const auto& ledger = index.ledger;
    const auto entries = [&](std::string_view key, Range range, bool first = false) {
        out << (first ? "" : ",");
        write_json_string(out, key);
        out << ':';
        write_ledger_entries(out, index, index.ledger_entries_in(range));
    };
    out << '{';
    entries("conf", ledger.conf, true);
    entries("env", ledger.env);
    entries("env_d", ledger.env_d);
    entries("globals", ledger.globals);
    entries("license_groups", ledger.license_groups);
    entries("package_accept_keywords", ledger.package_accept_keywords);
    entries("package_accept_restrict", ledger.package_accept_restrict);
    entries("package_keywords", ledger.package_keywords);
    entries("package_license", ledger.package_license);
    entries("package_mask", ledger.package_mask);
    entries("package_properties", ledger.package_properties);
    entries("package_unmask", ledger.package_unmask);
    out << ",\"profiles\":[";
    bool first = true;
    for (const auto& node : ledger.profiles) {
        out << (first ? "{" : ",{");
        first = false;
        entries("defaults", node.defaults, true);
        entries("package_accept_keywords", node.package_accept_keywords);
        entries("package_keywords", node.package_keywords);
        entries("package_license", node.package_license);
        entries("package_mask", node.package_mask);
        entries("package_unmask", node.package_unmask);
        out << ",\"path\":";
        write_string(out, index, node.path);
        out << '}';
    }
    out << "],\"repositories\":[";
    first = true;
    for (const auto& repo : ledger.repositories) {
        out << (first ? "{\"masters\":" : ",{\"masters\":");
        first = false;
        write_string_list(out, index, repo.masters);
        out << ",\"name\":";
        write_string(out, index, repo.name);
        entries("package_mask", repo.package_mask);
        entries("package_unmask", repo.package_unmask);
        out << '}';
    }
    out << "]}";
}

} // namespace

void write_repository_json(std::ostream& out, const RepositoryIndex& index) {
    out << R"({"advisories":[)";
    bool first = true;
    for (const auto& advisory : index.advisories) {
        out << (first ? "{\"id\":" : ",{\"id\":");
        first = false;
        write_string(out, index, advisory.id);
        out << ",\"packages\":[";
        bool first_package = true;
        for (const auto& package : index.packages_in(advisory.packages)) {
            out << (first_package ? "{\"arch\":" : ",{\"arch\":");
            first_package = false;
            write_string(out, index, package.arch);
            out << ",\"cp\":";
            write_string(out, index, package.cp);
            out << ",\"unaffected\":";
            write_string_list(out, index, package.unaffected);
            out << ",\"vulnerable\":";
            write_string_list(out, index, package.vulnerable);
            out << '}';
        }
        out << "],\"revision\":" << advisory.revision << ",\"synopsis\":";
        write_string(out, index, advisory.synopsis);
        out << ",\"title\":";
        write_string(out, index, advisory.title);
        out << '}';
    }
    out << R"(],"format":6,"ledger":)";
    write_visibility_ledger(out, index);
    out << R"(,"repositories":[)";
    first = true;
    for (const auto& repository : index.repositories) {
        out << (first ? "{" : ",{")
            << "\"description_index\":" << (repository.description_index ? "true" : "false")
            << ",\"location\":";
        first = false;
        write_string(out, index, repository.location);
        out << ",\"name\":";
        write_string(out, index, repository.name);
        out << '}';
    }
    out << R"(],"versions":[)";
    first = true;
    for (const auto& version : index.versions) {
        out << (first ? "{\"cp\":" : ",{\"cp\":");
        first = false;
        write_string(out, index, version.cp);
        out << ",\"cpv\":";
        write_string(out, index, version.cpv);
        out << ",\"description\":";
        write_string(out, index, version.description);
        out << ",\"eapi\":";
        write_string(out, index, version.eapi);
        out << ",\"homepage\":";
        write_string(out, index, version.homepage);
        out << ",\"invalid\":";
        write_string_list(out, index, version.invalid);
        out << ",\"keywords\":";
        write_string_list(out, index, version.keywords);
        out << ",\"license\":";
        write_string_list(out, index, version.license);
        out << ",\"properties\":";
        write_string_list(out, index, version.properties);
        out << ",\"repo\":";
        write_string(out, index, index.repositories.at(version.repository).name);
        out << ",\"restrict\":";
        write_string_list(out, index, version.restrict);
        out << ",\"slot\":";
        write_string(out, index, version.slot);
        out << ",\"sub_slot\":";
        write_string(out, index, version.sub_slot);
        out << ",\"use\":";
        write_string_list(out, index, version.use);
        out << '}';
    }
    const auto& vis = index.visibility;
    out << R"(],"visibility":{"arch":)";
    write_string(out, index, vis.arch);
    out << ",\"eapis\":[";
    first = true;
    for (const auto& eapi : vis.eapis) {
        out << (first ? "" : ",") << "{\"deprecated\":" << (eapi.deprecated ? "true" : "false")
            << ",\"eapi\":";
        first = false;
        write_string(out, index, eapi.eapi);
        out << ",\"supported\":" << (eapi.supported ? "true" : "false") << '}';
    }
    out << "]}}\n";
}

std::string package_json(const Store& store, const Package& pkg) {
    std::ostringstream out;
    write_package(out, store, pkg);
    return std::move(out).str();
}

namespace {

void write_root(std::ostream& out, const Store& store, const Root& root) {
    out << "{\"atom\":";
    write_string(out, store, root.atom);
    out << ",\"matches\":[";
    bool first = true;
    for (const auto id : store.ids_in(root.matches)) {
        out << (first ? "" : ",");
        first = false;
        write_string(out, store, store.packages.at(id).cpv);
    }
    out << "],\"set\":";
    write_string(out, store, root.set);
    out << ",\"via\":";
    write_string(out, store, root.via);
    out << '}';
}

// Roots go out whole, and with only_matching, only those matching one of the packages.
void write_document(std::ostream& out, const Store& store, std::span<const std::uint32_t> packages,
                    bool only_matching) {
    out << R"({"format":4,"packages":[)";
    std::vector<bool> chosen(store.packages.size(), false);
    bool first = true;
    for (const auto id : packages) {
        out << (first ? "" : ",");
        first = false;
        write_package(out, store, store.packages.at(id));
        chosen.at(id) = true;
    }
    out << R"(],"roots":[)";
    first = true;
    for (const auto& root : store.roots) {
        const auto ids = store.ids_in(root.matches);
        if (only_matching &&
            std::ranges::none_of(ids, [&](std::uint32_t id) { return chosen.at(id); })) {
            continue;
        }
        out << (first ? "" : ",");
        first = false;
        write_root(out, store, root);
    }
    out << "]}\n";
}

} // namespace

void write_json(std::ostream& out, const Store& store, std::span<const std::uint32_t> packages) {
    write_document(out, store, packages, true);
}

void write_json(std::ostream& out, const Store& store) {
    std::vector<std::uint32_t> all(store.packages.size());
    std::ranges::iota(all, 0U);
    write_document(out, store, all, false);
}

} // namespace egraph
