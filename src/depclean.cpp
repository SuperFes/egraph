#include "depclean.hpp"

#include "atom.hpp"
#include "query.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <iterator>
#include <numeric>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace egraph {

namespace {

// Indexes into dep_kinds in the order depclean reads them: RDEPEND, IDEPEND, PDEPEND, DEPEND,
// BDEPEND.
constexpr std::array<std::uint32_t, 5> kind_order{4, 2, 3, 1, 0};

template <class T> const T& element(std::span<const T> items, std::size_t index) {
    return items.subspan(index).front();
}

// What depclean's choice logic asks of one atom.
struct AtomFacts {
    bool is_virtual = false;
    // The installed package it selects when its USE dependencies are ignored is available.
    bool available = false;
    // Some installed package has its cp.
    bool cp_installed = false;
};

// One alternative of a || group, as dep_zapdeps classifies it.
struct Choice {
    // Atom node indexes, nested || groups already resolved.
    std::vector<std::uint32_t> atoms;
    bool available = true;
    bool use_satisfied = true;
    bool in_graph = true;
    bool all_installed = true;
    bool some_installed = false;
    bool cp_installed = false;
    // (cp string id, package): the highest installed package the choice selects in each cp.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> selects;
};

class Depclean {
  public:
    Depclean(const Store& store, const KeepOptions& options)
        : store_ref_(store), options_(options) {
        kept_.packages.assign(store.packages.size(), false);
        versions_.reserve(store.packages.size());
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            const auto& pkg = store.packages.at(id);
            if (!gone(id)) {
                by_cp_[store.string(pkg.cp)].push_back(id);
            }
            const auto cpv = store.string(pkg.cpv);
            const auto cp = store.string(pkg.cp);
            versions_.push_back(
                parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1))).value_or(Version{}));
        }
    }

    Kept run() {
        for (std::uint32_t root = 0; root < store().roots.size(); ++root) {
            const auto& found = store().roots.at(root);
            if (!options_.dropped.empty() && options_.dropped.at(root)) {
                continue;
            }
            // A set world_sets names stays, with its atoms.
            if (options_.without_selected && store().string(found.set) == "selected" &&
                store().string(found.via).empty()) {
                continue;
            }
            if (const auto child = select(store().ids_in(found.matches))) {
                kept_.roots.push_back({.root = root, .child = *child});
                add(*child);
            }
        }
        for (std::uint32_t pkg = 0; pkg < options_.protect.size(); ++pkg) {
            if (options_.protect.at(pkg) && !gone(pkg)) {
                add(pkg);
            }
        }
        // Like emerge: a queued package is read before any queued || group is resolved, so
        // choices see everything the plain dependencies keep.
        while (!stack_.empty() || !disjunctions_.empty()) {
            while (!stack_.empty()) {
                const auto pkg = stack_.back();
                stack_.pop_back();
                read(pkg);
            }
            if (!disjunctions_.empty()) {
                const auto disjunction = std::move(disjunctions_.back());
                disjunctions_.pop_back();
                resolve(disjunction);
            }
        }
        return std::move(kept_);
    }

  private:
    const Store& store() const { return store_ref_.get(); }

    // The || groups and virtual atoms of one dependency list, deferred together.
    struct Disjunction {
        std::uint32_t parent = 0;
        std::uint32_t kind = 0;
        std::vector<std::uint32_t> nodes;
    };

    std::span<const Node> nodes_of(std::uint32_t pkg, std::uint32_t kind) const {
        return store().nodes_in(store().packages.at(pkg).deps.at(kind));
    }

    int compare(std::uint32_t a, std::uint32_t b) const {
        return vercmp(versions_.at(a), versions_.at(b));
    }

    std::optional<std::uint32_t> highest(std::span<const std::uint32_t> ids) const {
        std::optional<std::uint32_t> best;
        for (const auto id : ids) {
            if (!best || compare(id, *best) > 0) {
                best = id;
            }
        }
        return best;
    }

    bool gone(std::uint32_t pkg) const {
        return !options_.removed.empty() && options_.removed.at(pkg);
    }

    const Masking& masking(std::uint32_t pkg) const {
        static constexpr Masking unmasked;
        return options_.masking.empty() ? unmasked : options_.masking.at(pkg);
    }

    // What the composite dbapi dep_zapdeps asks lets through: a masked installed package only
    // when an ebuild of its version is visible.
    bool usable(std::uint32_t pkg) const {
        const auto& found = masking(pkg);
        return !found.masked || found.visible;
    }

    // select_present over the ids not removed.
    std::optional<std::uint32_t> select(std::span<const std::uint32_t> ids) const {
        if (!options_.removed.empty()) {
            std::vector<std::uint32_t> left;
            std::ranges::copy_if(ids, std::back_inserter(left),
                                 [this](std::uint32_t id) { return !gone(id); });
            if (left.size() != ids.size()) {
                return select_present(left);
            }
        }
        return select_present(ids);
    }

    // _select_pkg_from_installed: of several matches, the unmasked ones, and of those the
    // visible ones, when there are any; then the highest.
    std::optional<std::uint32_t> select_present(std::span<const std::uint32_t> ids) const {
        if (ids.size() < 2 || options_.masking.empty()) {
            return highest(ids);
        }
        std::vector<std::uint32_t> kept;
        std::ranges::copy_if(ids, std::back_inserter(kept),
                             [&](std::uint32_t id) { return !masking(id).masked; });
        if (kept.empty()) {
            return highest(ids);
        }
        std::vector<std::uint32_t> visible;
        std::ranges::copy_if(kept, std::back_inserter(visible),
                             [&](std::uint32_t id) { return masking(id).visible; });
        return highest(visible.empty() ? kept : visible);
    }

    // A package the composite dbapi selects for ids, if it lets it through.
    bool selects_usable(std::span<const std::uint32_t> ids) const {
        const auto pkg = select(ids);
        return pkg && usable(*pkg);
    }

    AtomFacts facts(std::uint32_t atom_string) {
        if (const auto found = facts_.find(atom_string); found != facts_.end()) {
            return found->second;
        }
        AtomFacts facts;
        auto text = store().string(atom_string);
        // The installed packages it would match without its USE dependencies.
        if (text.ends_with(']')) {
            text = text.substr(0, text.rfind('['));
        }
        if (auto atom = parse_atom(text)) {
            facts.is_virtual = atom->cp.starts_with("virtual/");
            const auto installed = by_cp_.find(atom->cp);
            if (installed != by_cp_.end()) {
                facts.cp_installed = true;
                std::vector<std::uint32_t> ids;
                std::ranges::copy_if(installed->second, std::back_inserter(ids),
                                     [&](std::uint32_t id) {
                                         return matches(store(), store().packages.at(id), *atom);
                                     });
                facts.available = selects_usable(ids);
            }
        }
        facts_.emplace(atom_string, facts);
        return facts;
    }

    void add(std::uint32_t pkg) {
        if (!kept_.packages.at(pkg)) {
            kept_.packages.at(pkg) = true;
            stack_.push_back(pkg);
        }
    }

    // Keeps child for the atom at index, or records the atom as unresolved without one.
    void pull(std::uint32_t parent, std::uint32_t kind, std::uint32_t index, bool choice,
              std::optional<std::uint32_t> child) {
        const auto& node = element(nodes_of(parent, kind), index);
        if (child) {
            kept_.pulls.push_back({.parent = parent,
                                   .child = *child,
                                   .kind = kind,
                                   .atom = node.atom,
                                   .choice = choice});
            add(*child);
        } else if (!is_build_kind(kind)) {
            kept_.unresolved.push_back({.parent = parent, .kind = kind, .node = index});
        }
    }

    // What each atom of one dependency list keeps, as emerge's _minimize_children decides: each
    // atom selects the highest installed package it matches, and where the list's atoms select
    // several packages of one cp, those go lowest first while every atom matching them matches
    // another; each atom then keeps the highest package left that it matches.
    void pull_all(std::uint32_t parent, std::uint32_t kind,
                  const std::vector<std::pair<std::uint32_t, bool>>& atoms) {
        const auto nodes = nodes_of(parent, kind);
        std::vector<std::optional<std::uint32_t>> children;
        children.reserve(atoms.size());
        for (const auto& [index, choice] : atoms) {
            children.push_back(select(store().ids_in(element(nodes, index).matches)));
        }

        // The distinct packages selected in each cp.
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> selected;
        for (const auto& child : children) {
            if (child) {
                auto& pkgs = selected[store().packages.at(*child).cp];
                if (std::ranges::find(pkgs, *child) == pkgs.end()) {
                    pkgs.push_back(*child);
                }
            }
        }
        // Per distinct atom string, as emerge keys atoms, the selected packages it matches.
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> left;
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            const auto& child = children.at(i);
            if (!child) {
                continue;
            }
            const auto& pkgs = selected.at(store().packages.at(*child).cp);
            if (pkgs.size() < 2) {
                continue;
            }
            const auto& node = element(nodes, atoms.at(i).first);
            auto [found, added] = left.try_emplace(node.atom);
            if (added) {
                const auto matches = store().ids_in(node.matches);
                std::ranges::copy_if(
                    pkgs, std::back_inserter(found->second),
                    [&](std::uint32_t pkg) { return std::ranges::contains(matches, pkg); });
            }
        }
        for (auto& [cp, pkgs] : selected) {
            if (pkgs.size() < 2) {
                continue;
            }
            std::ranges::sort(pkgs,
                              [&](std::uint32_t a, std::uint32_t b) { return compare(a, b) < 0; });
            for (const auto pkg : pkgs) {
                const auto needed = std::ranges::any_of(left, [&](const auto& entry) {
                    return entry.second.size() < 2 && std::ranges::contains(entry.second, pkg);
                });
                if (!needed) {
                    for (auto& entry : left) {
                        std::erase(entry.second, pkg);
                    }
                }
            }
        }
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            const auto& [index, choice] = atoms.at(i);
            auto child = children.at(i);
            if (const auto found = left.find(element(nodes, index).atom); found != left.end()) {
                child = highest(found->second);
            }
            pull(parent, kind, index, choice, child);
        }
    }

    void read(std::uint32_t pkg) {
        for (const auto kind : kind_order) {
            if (is_build_kind(kind) && !options_.build_deps) {
                continue;
            }
            const auto nodes = nodes_of(pkg, kind);
            const auto inside = choices(nodes);
            Disjunction deferred{.parent = pkg, .kind = kind, .nodes = {}};
            std::vector<std::pair<std::uint32_t, bool>> plain;
            for (std::uint32_t i = 0; i < nodes.size(); ++i) {
                const auto& node = element(nodes, i);
                if (inside.at(i)) {
                    continue;
                }
                if (node.type == NodeType::any_of ||
                    (node.type == NodeType::atom && facts(node.atom).is_virtual)) {
                    deferred.nodes.push_back(i);
                } else if (node.type == NodeType::atom) {
                    plain.emplace_back(i, false);
                }
            }
            pull_all(pkg, kind, plain);
            if (!deferred.nodes.empty()) {
                disjunctions_.push_back(std::move(deferred));
            }
        }
    }

    void resolve(const Disjunction& disjunction) {
        const auto nodes = nodes_of(disjunction.parent, disjunction.kind);
        std::vector<std::pair<std::uint32_t, bool>> selected;
        for (const auto index : disjunction.nodes) {
            if (element(nodes, index).type == NodeType::any_of) {
                std::vector<std::uint32_t> atoms;
                choose(nodes, index, atoms);
                for (const auto atom : atoms) {
                    selected.emplace_back(atom, true);
                }
            } else {
                selected.emplace_back(index, false);
            }
        }
        // Every group of the list is decided before any of the choices is kept.
        pull_all(disjunction.parent, disjunction.kind, selected);
    }

    // The atoms of the group or atom at index, nested || groups resolved.
    void flatten(std::span<const Node> nodes, std::uint32_t index,
                 std::vector<std::uint32_t>& atoms) {
        const auto& node = element(nodes, index);
        switch (node.type) {
        case NodeType::atom:
            atoms.push_back(index);
            break;
        case NodeType::any_of:
            choose(nodes, index, atoms);
            break;
        case NodeType::all_of:
            for (std::uint32_t i = index + 1; i < nodes.size(); ++i) {
                if (element(nodes, i).parent == index) {
                    flatten(nodes, i, atoms);
                }
            }
            break;
        case NodeType::weak_blocker:
        case NodeType::strong_blocker:
            break;
        }
    }

    Choice classify(std::span<const Node> nodes, std::uint32_t index) {
        Choice choice;
        flatten(nodes, index, choice.atoms);
        for (const auto atom : choice.atoms) {
            const auto& node = element(nodes, atom);
            const auto atom_facts = facts(node.atom);
            const auto ids = store().ids_in(node.matches);
            choice.available = choice.available && atom_facts.available;
            choice.use_satisfied = choice.use_satisfied && selects_usable(ids);
            choice.all_installed = choice.all_installed && !ids.empty();
            choice.some_installed = choice.some_installed || !ids.empty();
            choice.cp_installed = choice.cp_installed || atom_facts.cp_installed;
            if (!atom_facts.is_virtual && std::ranges::none_of(ids, [&](std::uint32_t id) {
                    return kept_.packages.at(id);
                })) {
                choice.in_graph = false;
            }
            if (!choice.available) {
                // dep_zapdeps stops weighing versions at the first atom nothing available
                // matches, and counts only an available alternative as in the graph.
                choice.in_graph = false;
                continue;
            }
            if (const auto best = select(ids)) {
                const auto cp = store().packages.at(*best).cp;
                const auto known = std::ranges::find(
                    choice.selects, cp, &std::pair<std::uint32_t, std::uint32_t>::first);
                if (known == choice.selects.end()) {
                    choice.selects.emplace_back(cp, *best);
                } else if (compare(*best, known->second) > 0) {
                    known->second = *best;
                }
            }
        }
        return choice;
    }

    // dep_zapdeps's promotion within a bin: an alternative moves ahead of the first earlier one
    // it would upgrade, or that is not already kept when it is.
    void promote(std::vector<Choice>& bin) {
        if (bin.size() < 2) {
            return;
        }
        std::vector<std::size_t> order(bin.size());
        std::ranges::iota(order, 0U);
        for (std::size_t later = 1; later < bin.size(); ++later) {
            const auto& first = bin.at(later);
            for (std::size_t position = 0; order.at(position) != later; ++position) {
                const auto& second = bin.at(order.at(position));
                bool upgrade = false;
                bool downgrade = false;
                for (const auto& [cp, pkg] : first.selects) {
                    const auto other = std::ranges::find(
                        second.selects, cp, &std::pair<std::uint32_t, std::uint32_t>::first);
                    if (other == second.selects.end()) {
                        continue;
                    }
                    const auto order_of = compare(pkg, other->second);
                    upgrade = upgrade || order_of > 0;
                    downgrade = downgrade || order_of < 0;
                }
                if ((upgrade && !downgrade) ||
                    (first.in_graph && !second.in_graph && !(downgrade && !upgrade))) {
                    const auto from = std::ranges::find(order, later);
                    order.erase(from);
                    order.insert(order.begin() + static_cast<std::ptrdiff_t>(position), later);
                    break;
                }
            }
        }
        std::vector<Choice> sorted;
        sorted.reserve(bin.size());
        for (const auto index : order) {
            sorted.push_back(std::move(bin.at(index)));
        }
        bin = std::move(sorted);
    }

    // dep_zapdeps for one || group: an alternative whose atoms are all installed and available
    // wins, with USE dependencies met before without, and failing that one with all, then some
    // of them installed. Appends the chosen alternative's atoms.
    void choose(std::span<const Node> nodes, std::uint32_t group,
                std::vector<std::uint32_t>& atoms) {
        // Bins in the order dep_zapdeps tries them.
        std::array<std::vector<Choice>, 6> bins;
        for (std::uint32_t i = group + 1; i < nodes.size(); ++i) {
            if (element(nodes, i).parent != group) {
                continue;
            }
            auto choice = classify(nodes, i);
            std::size_t bin = 5;
            if (choice.available) {
                bin = choice.use_satisfied ? 0 : 1;
            } else if (choice.all_installed) {
                bin = 2;
            } else if (choice.some_installed) {
                bin = 3;
            } else if (choice.cp_installed) {
                bin = 4;
            }
            bins.at(bin).push_back(std::move(choice));
        }
        for (auto& bin : bins) {
            promote(bin);
        }
        for (const auto& bin : bins) {
            if (!bin.empty()) {
                atoms.insert(atoms.end(), bin.front().atoms.begin(), bin.front().atoms.end());
                return;
            }
        }
    }

    std::reference_wrapper<const Store> store_ref_;
    KeepOptions options_;
    Kept kept_;
    std::vector<std::uint32_t> stack_;
    std::vector<Disjunction> disjunctions_;
    std::vector<Version> versions_;
    std::unordered_map<std::string_view, std::vector<std::uint32_t>> by_cp_;
    std::unordered_map<std::uint32_t, AtomFacts> facts_;
};

} // namespace

Kept keep(const Store& store, const KeepOptions& options) {
    return Depclean(store, options).run();
}

std::vector<std::uint32_t> orphans(const Kept& kept) {
    std::vector<std::uint32_t> ids;
    for (std::uint32_t id = 0; id < kept.packages.size(); ++id) {
        if (!kept.packages.at(id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::optional<Path> why(const Kept& kept, std::uint32_t package) {
    // Runtime reasons before build-time ones when both keep a package.
    const auto rank = [](std::uint32_t kind) {
        return static_cast<std::size_t>(std::ranges::find(kind_order, kind) - kind_order.begin());
    };
    std::vector<Edge> pulls = kept.pulls;
    std::ranges::sort(pulls, [&](const Edge& a, const Edge& b) {
        return std::tuple{a.parent, rank(a.kind), a.child, a.atom, a.choice} <
               std::tuple{b.parent, rank(b.kind), b.child, b.atom, b.choice};
    });
    const auto [first, last] = std::ranges::unique(pulls);
    pulls.erase(first, last);

    // Breadth first from the roots; each package remembers how it was first reached.
    const auto count = kept.packages.size();
    std::vector<bool> seen(count, false);
    std::vector<std::optional<std::size_t>> by_root(count);
    std::vector<std::optional<std::size_t>> by_pull(count);
    std::vector<std::uint32_t> queue;
    for (std::size_t i = 0; i < kept.roots.size(); ++i) {
        const auto child = kept.roots.at(i).child;
        if (!seen.at(child)) {
            seen.at(child) = true;
            by_root.at(child) = i;
            queue.push_back(child);
        }
    }
    for (std::size_t head = 0; head < queue.size() && !seen.at(package); ++head) {
        const auto parent = queue.at(head);
        const auto from = std::ranges::lower_bound(pulls, parent, {}, &Edge::parent);
        for (auto pull = from; pull != pulls.end() && pull->parent == parent; ++pull) {
            if (!seen.at(pull->child)) {
                seen.at(pull->child) = true;
                by_pull.at(pull->child) = static_cast<std::size_t>(pull - pulls.begin());
                queue.push_back(pull->child);
            }
        }
    }
    if (!seen.at(package)) {
        return std::nullopt;
    }
    Path path;
    auto at = package;
    while (const auto pull = by_pull.at(at)) {
        path.edges.push_back(pulls.at(*pull));
        at = pulls.at(*pull).parent;
    }
    std::ranges::reverse(path.edges);
    path.root = kept.roots.at(by_root.at(at).value());
    return path;
}

std::vector<std::string> path_lines(const Store& store, const Path& path) {
    const auto& root = store.roots.at(path.root.root);
    std::vector<std::string> lines{
        std::format("@{}\t{}\t{}", store.string(root.set), store.string(root.atom),
                    store.string(store.packages.at(path.root.child).cpv))};
    for (const auto& edge : path.edges) {
        lines.push_back(edge_line(store, edge));
    }
    return lines;
}

std::vector<std::string> unresolved_lines(const Store& store, const Kept& kept) {
    std::vector<std::string> lines;
    for (const auto& item : kept.unresolved) {
        const auto& pkg = store.packages.at(item.parent);
        const auto nodes = store.nodes_in(pkg.deps.at(item.kind));
        lines.push_back(std::format("{}\t{}\t{}", store.string(pkg.cpv), dep_kinds.at(item.kind),
                                    store.string(element(nodes, item.node).atom)));
    }
    std::ranges::sort(lines);
    const auto [first, last] = std::ranges::unique(lines);
    lines.erase(first, last);
    return lines;
}

} // namespace egraph
