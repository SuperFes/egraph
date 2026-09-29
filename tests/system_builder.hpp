#pragma once

// Installed and evaluated stores built from dependency strings, for tests of what reads both.

#include "atom.hpp"
#include "evaluated.hpp"
#include "store.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace egraph::test {

// Dependency strings by kind ("RDEPEND"): atoms, "|| ( ... )" and "( ... )" groups, "!" and
// "!!" blockers, whitespace-separated.
using DepStrings = std::map<std::string, std::string, std::less<>>;

struct Installed {
    std::string cpv;
    DepStrings deps = {};
    std::string slot = "0";
    std::string sub_slot = {};
};

struct Available {
    std::string cpv;
    DepStrings deps = {};
    std::string slot = "0";
    std::string sub_slot = {};
    bool visible = true;
    std::string repo = "test_repo";
};

struct System {
    Store store;
    Evaluated evaluated;
};

namespace detail {

inline std::uint32_t intern(Tables& tables, std::map<std::string, std::uint32_t, std::less<>>& ids,
                            std::string_view text) {
    if (const auto found = ids.find(text); found != ids.end()) {
        return found->second;
    }
    const auto id = static_cast<std::uint32_t>(tables.strings.size());
    tables.strings.push_back({.first = static_cast<std::uint32_t>(tables.pool.size()),
                              .count = static_cast<std::uint32_t>(text.size())});
    tables.pool += text;
    ids.emplace(std::string(text), id);
    return id;
}

class Interner {
  public:
    explicit Interner(Tables& tables) : tables_(tables) { (*this)(""); }
    std::uint32_t operator()(std::string_view text) { return intern(tables_.get(), ids_, text); }

  private:
    std::reference_wrapper<Tables> tables_;
    std::map<std::string, std::uint32_t, std::less<>> ids_;
};

inline std::vector<std::string> tokens(std::string_view text) {
    std::vector<std::string> found;
    std::istringstream in{std::string(text)};
    for (std::string token; in >> token;) {
        found.push_back(token);
    }
    return found;
}

// Installed package ids an atom (or a blocker's atom) matches.
using Matcher = std::function<std::vector<std::uint32_t>(std::string_view)>;

inline void parse_nodes(const std::vector<std::string>& words, std::size_t& at,
                        std::uint32_t parent, std::uint32_t first, Tables& tables,
                        Interner& strings, const Matcher& match) {
    while (at < words.size() && words.at(at) != ")") {
        const auto& word = words.at(at++);
        Node node;
        node.parent = parent;
        if (word == "||" || word == "(") {
            node.type = word == "||" ? NodeType::any_of : NodeType::all_of;
            if (word == "||") {
                ++at;
            }
            const auto index = static_cast<std::uint32_t>(tables.nodes.size()) - first;
            tables.nodes.push_back(node);
            parse_nodes(words, at, index, first, tables, strings, match);
            ++at;
            continue;
        }
        std::string_view atom = word;
        if (atom.starts_with("!!")) {
            node.type = NodeType::strong_blocker;
            atom.remove_prefix(2);
        } else if (atom.starts_with('!')) {
            node.type = NodeType::weak_blocker;
            atom.remove_prefix(1);
        }
        node.atom = strings(word);
        const auto ids = match(atom);
        node.matches = {.first = static_cast<std::uint32_t>(tables.ids.size()),
                        .count = static_cast<std::uint32_t>(ids.size())};
        tables.ids.insert(tables.ids.end(), ids.begin(), ids.end());
        tables.nodes.push_back(node);
    }
}

inline std::array<Range, dep_kinds.size()> trees(const DepStrings& deps, Tables& tables,
                                                 Interner& strings, const Matcher& match) {
    std::array<Range, dep_kinds.size()> found{};
    for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
        const auto first = static_cast<std::uint32_t>(tables.nodes.size());
        if (const auto text = deps.find(dep_kinds.at(kind)); text != deps.end()) {
            const auto words = tokens(text->second);
            std::size_t at = 0;
            parse_nodes(words, at, no_parent, first, tables, strings, match);
        }
        found.at(kind) = {.first = first,
                          .count = static_cast<std::uint32_t>(tables.nodes.size()) - first};
    }
    return found;
}

inline std::string cp_of(std::string_view cpv) {
    const auto atom = parse_atom("=" + std::string(cpv));
    if (!atom) {
        throw std::invalid_argument("not a cpv: " + std::string(cpv));
    }
    return atom->cp;
}

inline Version version_of(std::string_view cpv) {
    return *parse_version(cpv.substr(cp_of(cpv).size() + 1));
}

} // namespace detail

// Installed packages' dependencies are their evaluated ones too. An installed package's target
// is the best visible candidate in its slot, when that is newer, or any other when no visible
// candidate has its version.
inline System make_system(const std::vector<Installed>& installed,
                          std::vector<Available> available) {
    System system;
    auto& store = system.store;
    auto& evaluated = system.evaluated;
    detail::Interner store_strings(store);
    detail::Interner evaluated_strings(evaluated);
    for (const auto& pkg : installed) {
        Package record;
        record.cpv = store_strings(pkg.cpv);
        record.cp = store_strings(detail::cp_of(pkg.cpv));
        record.slot = store_strings(pkg.slot);
        record.sub_slot = store_strings(pkg.sub_slot.empty() ? pkg.slot : pkg.sub_slot);
        record.repo = store_strings("test_repo");
        record.eapi = store_strings("8");
        record.iuse_effective = true;
        store.packages.push_back(record);
    }
    const detail::Matcher match = [&store](std::string_view text) {
        std::vector<std::uint32_t> ids;
        const auto atom = parse_atom(text);
        if (!atom) {
            throw std::invalid_argument("not an atom: " + std::string(text));
        }
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (matches(store, store.packages.at(id), *atom)) {
                ids.push_back(id);
            }
        }
        return ids;
    };
    for (std::size_t i = 0; i < installed.size(); ++i) {
        store.packages.at(i).deps =
            detail::trees(installed.at(i).deps, store, store_strings, match);
    }

    std::ranges::sort(available, [](const Available& a, const Available& b) {
        return std::tie(a.cpv, a.repo) < std::tie(b.cpv, b.repo);
    });
    std::ranges::stable_sort(available, [](const Available& a, const Available& b) {
        return detail::cp_of(a.cpv) < detail::cp_of(b.cpv);
    });
    for (const auto& ebuild : available) {
        Candidate candidate;
        candidate.cp = evaluated_strings(detail::cp_of(ebuild.cpv));
        candidate.cpv = evaluated_strings(ebuild.cpv);
        candidate.repo = evaluated_strings(ebuild.repo);
        candidate.slot = evaluated_strings(ebuild.slot);
        candidate.sub_slot =
            evaluated_strings(ebuild.sub_slot.empty() ? ebuild.slot : ebuild.sub_slot);
        if (!ebuild.visible) {
            candidate.reasons = {.first = static_cast<std::uint32_t>(evaluated.ids.size()),
                                 .count = 1};
            evaluated.ids.push_back(evaluated_strings("package.mask"));
        } else {
            candidate.deps = detail::trees(ebuild.deps, evaluated, evaluated_strings, match);
        }
        evaluated.candidates.push_back(candidate);
    }

    for (const auto& pkg : installed) {
        Dependencies record;
        record.cpv = evaluated_strings(pkg.cpv);
        record.source = DepSource::ebuild;
        record.eapi = evaluated_strings("8");
        record.deps = detail::trees(pkg.deps, evaluated, evaluated_strings, match);
        const auto cp = detail::cp_of(pkg.cpv);
        const auto own = detail::version_of(pkg.cpv);
        record.visible = std::ranges::any_of(available, [&](const Available& ebuild) {
            return ebuild.visible && ebuild.cpv == pkg.cpv;
        });
        std::optional<std::uint32_t> best;
        for (std::uint32_t i = 0; i < available.size(); ++i) {
            const auto& ebuild = available.at(i);
            if (!ebuild.visible || ebuild.slot != pkg.slot || detail::cp_of(ebuild.cpv) != cp) {
                continue;
            }
            if (!best || vercmp(detail::version_of(ebuild.cpv),
                                detail::version_of(available.at(*best).cpv)) > 0) {
                best = i;
            }
        }
        if (best) {
            const int order = vercmp(detail::version_of(available.at(*best).cpv), own);
            if (order > 0 || (!record.visible && available.at(*best).cpv != pkg.cpv)) {
                record.target = best;
            }
        }
        evaluated.packages.push_back(record);
    }
    return system;
}

} // namespace egraph::test
