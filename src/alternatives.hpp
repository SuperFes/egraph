#pragma once

// emerge's dep_zapdeps preference among a || group's alternatives under -uD, shared by the
// reach and the plan.

#include "atom.hpp"
#include "store.hpp"
#include "version.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace egraph::alternatives {

// How the caller sees a dependency list: atom(node) -> std::optional<Atom>, the atom of an atom
// node; best(atom) -> std::optional<std::uint32_t>, the best visible candidate it matches;
// installed(cp) -> bool, whether some slot of cp is installed; version(candidate) ->
// std::optional<Version>.
template <typename AtomOf, typename Best, typename Installed, typename VersionOf> struct View {
    AtomOf atom;
    Best best;
    Installed installed;
    VersionOf version;
};

using CpVersions = std::map<std::string, Version, std::less<>>;

namespace detail {

inline const Node& at(std::span<const Node> list, std::size_t index) {
    return list.subspan(index, 1).front();
}

inline std::vector<std::size_t> children(std::span<const Node> list, std::size_t index) {
    std::vector<std::size_t> found;
    for (std::size_t child = index + 1; child < list.size(); ++child) {
        if (at(list, child).parent == index) {
            found.push_back(child);
        }
    }
    return found;
}

} // namespace detail

// dep_zapdeps' cp_map for the alternative at node index: by cp, the highest of its atoms' best
// visible versions, the best meeting the atom's USE dependencies where one does. None when an
// atom has no visible version, or a nested || is beyond this view; and with preferred, unless
// the alternative is in the bin dep_zapdeps prefers: its USE dependencies met and each of its
// packages installed in some slot.
template <typename View>
std::optional<CpVersions> versions(std::span<const Node> list, std::size_t index, bool preferred,
                                   View& view) {
    std::vector<std::size_t> atoms;
    std::vector<std::size_t> open{index};
    while (!open.empty()) {
        const auto node = open.back();
        open.pop_back();
        const auto type = detail::at(list, node).type;
        if (type == NodeType::atom) {
            atoms.push_back(node);
        } else if (type == NodeType::all_of) {
            for (const auto child : detail::children(list, node)) {
                open.push_back(child);
            }
        } else if (type == NodeType::any_of) {
            return std::nullopt;
        }
    }
    CpVersions found;
    for (const auto node : atoms) {
        const std::optional<Atom> wanted = view.atom(node);
        if (!wanted) {
            return std::nullopt;
        }
        auto best = view.best(*wanted);
        if (!best && !preferred) {
            auto plain = *wanted;
            plain.use.clear();
            best = view.best(plain);
        }
        if (!best ||
            (preferred && !wanted->cp.starts_with("virtual/") && !view.installed(wanted->cp))) {
            return std::nullopt;
        }
        auto version = view.version(*best);
        if (!version) {
            return std::nullopt;
        }
        const auto had = found.find(wanted->cp);
        if (had == found.end()) {
            found.emplace(wanted->cp, std::move(*version));
        } else if (vercmp(*version, had->second) > 0) {
            had->second = std::move(*version);
        }
    }
    return found;
}

// The alternative of the || at node index that dep_zapdeps takes under -uD over satisfied, the
// first one satisfied: an earlier one that would update (so satisfied is not promoted for being
// installed), which moves a package they share to a higher version and none to a lower (so
// satisfied is not promoted as an upgrade or for being in the graph).
template <typename View>
std::optional<std::size_t> upgrade(std::span<const Node> list, std::size_t index,
                                   std::size_t satisfied, View& view) {
    if (detail::at(list, index).type != NodeType::any_of) {
        return std::nullopt;
    }
    const auto held = versions(list, satisfied, false, view);
    if (!held) {
        return std::nullopt;
    }
    for (const auto child : detail::children(list, index)) {
        if (child == satisfied) {
            break;
        }
        const auto offered = versions(list, child, true, view);
        if (!offered) {
            continue;
        }
        bool higher = false;
        bool lower = false;
        for (const auto& [cp, version] : *offered) {
            if (const auto found = held->find(cp); found != held->end()) {
                const auto order = vercmp(version, found->second);
                higher = higher || order > 0;
                lower = lower || order < 0;
            }
        }
        if (higher && !lower) {
            return child;
        }
    }
    return std::nullopt;
}

} // namespace egraph::alternatives
