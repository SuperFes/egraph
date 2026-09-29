#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
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
    out << "{\"cp\":";
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
    out << ",\"provides\":";
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
        out << ",\"errors\":";
        write_pairs(out, evaluated, candidate.errors);
        out << ",\"iuse\":";
        write_string_list(out, evaluated, candidate.iuse);
        out << ",\"reasons\":";
        write_string_list(out, evaluated, candidate.reasons);
        out << ",\"repo\":";
        write_string(out, evaluated, candidate.repo);
        out << ",\"slot\":";
        write_string(out, evaluated, candidate.slot);
        out << ",\"sub_slot\":";
        write_string(out, evaluated, candidate.sub_slot);
        out << ",\"use\":";
        write_string_list(out, evaluated, candidate.use);
        out << '}';
    }
    out << R"(],"format":4,"packages":[)";
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
        out << ",\"vdb_masked\":" << (pkg.vdb_masked ? "true" : "false")
            << ",\"visible\":" << (pkg.visible ? "true" : "false") << '}';
    }
    out << "]}\n";
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
    out << '}';
}

// Roots go out whole, and with only_matching, only those matching one of the packages.
void write_document(std::ostream& out, const Store& store, std::span<const std::uint32_t> packages,
                    bool only_matching) {
    out << R"({"format":2,"packages":[)";
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
