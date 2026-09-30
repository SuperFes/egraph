#include "plan.hpp"

#include "atom.hpp"
#include "graph.hpp"
#include "version.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace egraph {

namespace {

const Node& element(std::span<const Node> nodes, std::size_t index) {
    return nodes.subspan(index, 1).front();
}

// Where a slot is taken: by cp and slot, both as the tables spell them.
using SlotKey = std::pair<std::string, std::string>;

std::optional<Version> version_of(std::string_view cpv, std::string_view cp) {
    return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
}

class Planner {
  public:
    Planner(const Store& store EGRAPH_KEPT_BY_THIS, const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
            UseRebuilds rebuilds, const Targets& targets EGRAPH_KEPT_BY_THIS)
        : store_ref_(store), evaluated_ref_(evaluated), targets_ref_(targets),
          choices_(store.packages.size()) {
        for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
            by_cp_[std::string(evaluated.string(evaluated.candidates.at(i).cp))].push_back(i);
        }
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (!in_scope(id)) {
                continue;
            }
            if (auto wanted = pending_update(evaluated, id, rebuilds)) {
                auto& choice = choices_.at(id);
                choice.options.push_back(wanted->target);
                std::ranges::copy(fallbacks(evaluated, id, *wanted),
                                  std::back_inserter(choice.options));
                choice.wanted = std::move(wanted);
            }
        }
        if (targets.roots) {
            arguments_.assign(store.packages.size(), false);
            for (const auto& root : store.roots) {
                for (const auto id : store.ids_in(root.matches)) {
                    arguments_.at(id) = true;
                }
            }
            for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
                if (in_scope(id) && !choices_.at(id).wanted) {
                    if (auto found = new_slot(id)) {
                        new_slots_.emplace(id, std::move(*found));
                    }
                }
            }
            for (const auto& [id, moved] : new_slots_) {
                induce(moved);
            }
        }
    }

    Plan run() {
        // Choices only move down their options, so this ends.
        while (settle()) {
        }
        return result();
    }

  private:
    struct Choice {
        std::optional<PendingUpdate> wanted;
        // The target, then its fallbacks.
        std::vector<std::uint32_t> options;
        // Index into options; options.size() keeps the installed package.
        std::size_t at = 0;
        // What rejected the target.
        std::vector<Reason> reasons;
        // Wanted only for a dependent's move to a newer slot, so never held.
        bool induced = false;

        [[nodiscard]] std::optional<std::uint32_t> merged() const {
            return at < options.size() ? std::optional{options.at(at)} : std::nullopt;
        }
    };

    struct Pulled {
        std::uint32_t candidate = 0;
        Reason by;
        // The installed package whose merge pulled it in, if a merge did.
        std::optional<std::uint32_t> root;
    };

    struct Rebuilt {
        std::uint32_t candidate = 0;
        Reason why;
    };

    // A slot-operator atom's binding that a merge breaks.
    struct Broken {
        // The installed package the merge replaces.
        std::uint32_t replaced = 0;
        std::uint32_t merged = 0;
        std::string atom;
    };

    // A slot-operator dependent's rebuild against a newer slot than the one it is bound to, as
    // emerge --rebuild-if-new-slot.
    struct NewSlot {
        // The best visible version in a newer slot that the rebuild's atoms accept.
        std::uint32_t candidate = 0;
        // The rebuild.
        std::uint32_t own = 0;
        // The installed package in the candidate's slot.
        std::optional<std::uint32_t> installed;
        // The bound atom, and the first atom of the rebuild that the candidate satisfies.
        std::string bound;
        std::string wanted;
        std::vector<Atom> atoms;
    };

    // A member whose dependencies the plan must satisfy.
    struct Work {
        Member member;
        std::optional<std::uint32_t> root;
    };

    std::reference_wrapper<const Store> store_ref_;
    std::reference_wrapper<const Evaluated> evaluated_ref_;
    std::reference_wrapper<const Targets> targets_ref_;
    std::vector<Choice> choices_;
    // Installed packages that emerge's arguments name; every one when empty.
    std::vector<bool> arguments_;
    std::map<std::uint32_t, NewSlot> new_slots_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp_;
    std::map<std::string, std::optional<Atom>, std::less<>> atoms_;
    // Slot and sub-slot a slot-operator atom is bound to, by the atom's text.
    std::map<std::string, std::optional<SlotKey>, std::less<>> bindings_;

    // The state of one pass, rebuilt from the choices.
    std::vector<Pulled> pulled_;
    // Merged and pulled candidates by cp.
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> present_;
    std::map<SlotKey, bool> taken_;
    // Slot-operator rebuilds by installed package.
    std::map<std::uint32_t, Rebuilt> rebuilt_;

    [[nodiscard]] const Store& store() const { return store_ref_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_ref_.get(); }

    [[nodiscard]] bool in_scope(std::uint32_t id) const {
        const auto& scope = targets_ref_.get().scope;
        return scope.empty() || scope.at(id);
    }

    [[nodiscard]] bool argument(std::uint32_t id) const {
        return arguments_.empty() || arguments_.at(id);
    }

    const std::optional<Atom>& atom(std::string_view text) {
        auto found = atoms_.find(text);
        if (found == atoms_.end()) {
            auto parsed = parse_atom(text);
            std::optional<Atom> value;
            if (parsed) {
                // A slot operator's sub-slot means a rebuild, not a bound.
                if (parsed->slot_operator) {
                    parsed->sub_slot.reset();
                }
                value = std::move(*parsed);
            }
            found = atoms_.emplace(std::string(text), std::move(value)).first;
        }
        return found->second;
    }

    const std::optional<SlotKey>& binding(std::string_view text) {
        auto found = bindings_.find(text);
        if (found == bindings_.end()) {
            std::optional<SlotKey> value;
            if (auto parsed = parse_atom(text);
                parsed && parsed->slot_operator && parsed->slot && parsed->sub_slot) {
                value = SlotKey{*parsed->slot, *parsed->sub_slot};
            }
            found = bindings_.emplace(std::string(text), std::move(value)).first;
        }
        return found->second;
    }

    // The bindings of the installed package's slot-operator atoms that merges break: every
    // package the atom matches is replaced, by a version in another slot or sub-slot.
    std::vector<Broken> broken_bindings(std::uint32_t id) {
        std::vector<Broken> found;
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            for (const auto& node : nodes({.candidate = false, .index = id}, kind)) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                const auto text = store().string(node.atom);
                const auto& bound = binding(text);
                const auto ids = store().ids_in(node.matches);
                if (!bound || ids.empty() ||
                    std::ranges::any_of(ids, [this](std::uint32_t match) { return kept(match); })) {
                    continue;
                }
                for (const auto match : ids) {
                    const auto merged = choices_.at(match).merged();
                    if (!merged) {
                        continue;
                    }
                    const auto& candidate = evaluated().candidates.at(*merged);
                    if (evaluated().string(candidate.slot) != bound->first ||
                        evaluated().string(candidate.sub_slot) != bound->second) {
                        found.push_back(
                            {.replaced = match, .merged = *merged, .atom = std::string(text)});
                        break;
                    }
                }
            }
        }
        return found;
    }

    // The move to a newer slot of the first slot-operator atom of the installed package, outside
    // any ||, that has one.
    std::optional<NewSlot> new_slot(std::uint32_t id) {
        const auto own = rebuild_of(id);
        if (!own) {
            return std::nullopt;
        }
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            const auto list = nodes({.candidate = false, .index = id}, kind);
            const auto inside = choices(list);
            for (std::size_t i = 0; i < list.size(); ++i) {
                const auto& node = element(list, i);
                const auto text = store().string(node.atom);
                if (node.type != NodeType::atom || inside.at(i) || !binding(text)) {
                    continue;
                }
                for (const auto child : store().ids_in(node.matches)) {
                    if (auto found = newer_slot(id, child, *own, text)) {
                        return found;
                    }
                }
            }
        }
        return std::nullopt;
    }

    // What emerge's slot-operator update probe finds for parent's binding to child: the best
    // visible version of child in a newer slot, higher than child, that every atom of the
    // rebuild naming child's cp accepts (none naming child's slot), and that every other
    // dependent and root atom of child accepts, a slot-operator one whatever its slot.
    std::optional<NewSlot> newer_slot(std::uint32_t parent, std::uint32_t child, std::uint32_t own,
                                      std::string_view bound) {
        const auto& pkg = store().packages.at(child);
        const auto cp = store().string(pkg.cp);
        const auto slot = store().string(pkg.slot);
        NewSlot found{.candidate = 0,
                      .own = own,
                      .installed = {},
                      .bound = std::string(bound),
                      .wanted = {},
                      .atoms = {}};
        std::vector<std::string> texts;
        for (const auto range : evaluated().candidates.at(own).deps) {
            const auto list = evaluated().nodes_in(range);
            const auto inside = choices(list);
            for (std::size_t i = 0; i < list.size(); ++i) {
                const auto& node = element(list, i);
                if (node.type != NodeType::atom || inside.at(i)) {
                    continue;
                }
                auto parsed = parse_atom(evaluated().string(node.atom));
                if (!parsed || parsed->cp != cp) {
                    continue;
                }
                if (parsed->slot == slot) {
                    return std::nullopt;
                }
                // USE dependencies are too involved for the probe, as for emerge's.
                parsed->use.clear();
                parsed->sub_slot.reset();
                found.atoms.push_back(std::move(*parsed));
                texts.emplace_back(evaluated().string(node.atom));
            }
        }
        const auto same = by_cp_.find(cp);
        const auto installed_version = version_of(store().string(pkg.cpv), cp);
        if (found.atoms.empty() || same == by_cp_.end() || !installed_version) {
            return std::nullopt;
        }
        std::optional<Version> best;
        for (const auto index : same->second) {
            const auto& candidate = evaluated().candidates.at(index);
            auto version = version_of(evaluated().string(candidate.cpv), cp);
            if (!candidate.visible() || evaluated().string(candidate.slot) == slot || !version ||
                vercmp(*version, *installed_version) <= 0 ||
                (best && vercmp(*version, *best) <= 0) || !accepts(found.atoms, index)) {
                continue;
            }
            found.candidate = index;
            best = std::move(version);
        }
        if (!best || !others_accept(parent, child, found.candidate)) {
            return std::nullopt;
        }
        const auto& candidate = evaluated().candidates.at(found.candidate);
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            const auto& other = store().packages.at(id);
            if (store().string(other.cp) != cp ||
                store().string(other.slot) != evaluated().string(candidate.slot)) {
                continue;
            }
            const auto version = version_of(store().string(other.cpv), cp);
            if (!version || vercmp(*version, *best) > 0) {
                return std::nullopt;
            }
            found.installed = id;
        }
        for (std::size_t i = 0; i < found.atoms.size(); ++i) {
            if (matches(store(), evaluated(), candidate, found.atoms.at(i))) {
                found.wanted = texts.at(i);
                break;
            }
        }
        return found;
    }

    [[nodiscard]] bool accepts(const std::vector<Atom>& atoms, std::uint32_t candidate) const {
        return std::ranges::all_of(atoms, [&](const Atom& wanted) {
            return matches(store(), evaluated(), evaluated().candidates.at(candidate), wanted);
        });
    }

    [[nodiscard]] bool others_accept(std::uint32_t parent, std::uint32_t child,
                                     std::uint32_t candidate) const {
        const auto& replacement = evaluated().candidates.at(candidate);
        const auto accepted = [&](std::string_view text) {
            auto parsed = parse_atom(text);
            if (parsed && parsed->slot_operator) {
                parsed->slot.reset();
                parsed->sub_slot.reset();
            }
            return !parsed || matches(store(), evaluated(), replacement, *parsed);
        };
        const auto names = [&](Range ids) {
            return std::ranges::contains(store().ids_in(ids), child);
        };
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            // A dependent that is updated brings dependencies of its own.
            if (id == parent || !in_scope(id) || choices_.at(id).wanted) {
                continue;
            }
            for (const auto range : store().packages.at(id).deps) {
                for (const auto& node : store().nodes_in(range)) {
                    if (node.type == NodeType::atom && names(node.matches) &&
                        !accepted(store().string(node.atom))) {
                        return false;
                    }
                }
            }
        }
        return std::ranges::all_of(store().roots, [&](const Root& root) {
            return !names(root.matches) || accepted(store().string(root.atom));
        });
    }

    // Gives the installed package in a newer slot, when nothing else updates it, the update
    // the move needs.
    void induce(const NewSlot& moved) {
        if (!moved.installed) {
            return;
        }
        auto& choice = choices_.at(*moved.installed);
        const auto& from = store().packages.at(*moved.installed);
        const auto& to = evaluated().candidates.at(moved.candidate);
        if (choice.wanted || store().string(from.cpv) == evaluated().string(to.cpv)) {
            return;
        }
        choice.options = {moved.candidate};
        choice.wanted =
            PendingUpdate{.kind = update_kind(store().string(from.cp), store().string(from.cpv),
                                              evaluated().string(to.cpv)),
                          .target = moved.candidate,
                          .flags = {}};
        choice.induced = true;
    }

    // What the dependent moves to in this pass, pulling the candidate in when its slot is free;
    // nullopt when the newer slot's package stays behind.
    std::optional<Member> moved_to(std::uint32_t id, const NewSlot& moved,
                                   std::vector<Work>& work) {
        if (moved.installed) {
            if (const auto merged = choices_.at(*moved.installed).merged()) {
                return accepts(moved.atoms, *merged)
                           ? std::optional{Member{.candidate = true, .index = *merged}}
                           : std::nullopt;
            }
            return store().string(store().packages.at(*moved.installed).cpv) ==
                           evaluated().string(evaluated().candidates.at(moved.candidate).cpv)
                       ? std::optional{Member{.candidate = false, .index = *moved.installed}}
                       : std::nullopt;
        }
        const auto& candidate = evaluated().candidates.at(moved.candidate);
        const auto cp = evaluated().string(candidate.cp);
        const auto slot = evaluated().string(candidate.slot);
        if (taken_.contains({std::string(cp), std::string(slot)})) {
            for (const auto other : present_.at(std::string(cp))) {
                if (evaluated().string(evaluated().candidates.at(other).slot) == slot) {
                    return accepts(moved.atoms, other)
                               ? std::optional{Member{.candidate = true, .index = other}}
                               : std::nullopt;
                }
            }
        }
        place(moved.candidate);
        pulled_.push_back(
            {.candidate = moved.candidate,
             .by = {.member = {.candidate = false, .index = id}, .atom = moved.wanted},
             .root = {}});
        work.push_back({.member = {.candidate = true, .index = moved.candidate}, .root = {}});
        return Member{.candidate = true, .index = moved.candidate};
    }

    // A visible ebuild of the installed package's version, from its repository if it can.
    [[nodiscard]] std::optional<std::uint32_t> rebuild_of(std::uint32_t id) const {
        const auto& pkg = store().packages.at(id);
        const auto found = by_cp_.find(store().string(pkg.cp));
        if (found == by_cp_.end()) {
            return std::nullopt;
        }
        std::optional<std::uint32_t> best;
        for (const auto index : found->second) {
            const auto& candidate = evaluated().candidates.at(index);
            if (!candidate.visible() ||
                evaluated().string(candidate.cpv) != store().string(pkg.cpv)) {
                continue;
            }
            if (evaluated().string(candidate.repo) == store().string(pkg.repo)) {
                return index;
            }
            best = best.value_or(index);
        }
        return best;
    }

    [[nodiscard]] const Tables& tables(const Member& member) const {
        return member.candidate ? static_cast<const Tables&>(evaluated())
                                : static_cast<const Tables&>(store());
    }

    [[nodiscard]] std::span<const Node> nodes(const Member& member, std::size_t kind) const {
        if (member.candidate) {
            return evaluated().nodes_in(evaluated().candidates.at(member.index).deps.at(kind));
        }
        return store().nodes_in(store().packages.at(member.index).deps.at(kind));
    }

    [[nodiscard]] bool kept(std::uint32_t id) const { return !choices_.at(id).merged(); }

    void place(std::uint32_t candidate) {
        const auto& c = evaluated().candidates.at(candidate);
        present_[std::string(evaluated().string(c.cp))].push_back(candidate);
        taken_[{std::string(evaluated().string(c.cp)), std::string(evaluated().string(c.slot))}] =
            true;
    }

    void unplace(std::uint32_t candidate) {
        const auto& c = evaluated().candidates.at(candidate);
        auto& list = present_[std::string(evaluated().string(c.cp))];
        list.erase(std::ranges::find(list, candidate));
        taken_.erase(
            {std::string(evaluated().string(c.cp)), std::string(evaluated().string(c.slot))});
    }

    // Whether the atom node is satisfied by what the plan holds.
    bool satisfied_atom(const Tables& tables, const Node& node) {
        if (std::ranges::any_of(tables.ids_in(node.matches),
                                [this](std::uint32_t id) { return kept(id); })) {
            return true;
        }
        const auto& wanted = atom(tables.string(node.atom));
        if (!wanted) {
            return true;
        }
        const auto found = present_.find(wanted->cp);
        return found != present_.end() &&
               std::ranges::any_of(found->second, [&](std::uint32_t candidate) {
                   return matches(store(), evaluated(), evaluated().candidates.at(candidate),
                                  *wanted);
               });
    }

    std::vector<bool> satisfied_nodes(const Tables& tables, std::span<const Node> list) {
        return satisfied(list, [&](std::size_t i) {
            const auto& node = element(list, i);
            return node.type != NodeType::atom || satisfied_atom(tables, node);
        });
    }

    // The best visible candidate that matches the atom, if its slot is free.
    std::optional<std::uint32_t> pullable(const Atom& wanted) {
        const auto found = by_cp_.find(wanted.cp);
        if (found == by_cp_.end()) {
            return std::nullopt;
        }
        std::optional<std::uint32_t> best;
        std::optional<Version> best_version;
        for (const auto index : found->second) {
            const auto& candidate = evaluated().candidates.at(index);
            if (!candidate.visible() || !matches(store(), evaluated(), candidate, wanted)) {
                continue;
            }
            auto version = version_of(evaluated().string(candidate.cpv), wanted.cp);
            if (version && (!best_version || vercmp(*version, *best_version) > 0)) {
                best = index;
                best_version = std::move(version);
            }
        }
        if (!best) {
            return std::nullopt;
        }
        const auto& candidate = evaluated().candidates.at(*best);
        const SlotKey key{std::string(evaluated().string(candidate.cp)),
                          std::string(evaluated().string(candidate.slot))};
        if (taken_.contains(key) || installed_in(key)) {
            return std::nullopt;
        }
        return best;
    }

    [[nodiscard]] bool installed_in(const SlotKey& key) const {
        return std::ranges::any_of(store().packages, [&](const Package& pkg) {
            return store().string(pkg.cp) == key.first && store().string(pkg.slot) == key.second;
        });
    }

    // Pulls in what node index of list needs, appending to pulls; false, with nothing placed,
    // when it cannot be satisfied.
    bool pull(const Tables& tables, std::span<const Node> list, std::size_t index,
              std::vector<std::pair<std::uint32_t, std::string>>& pulls) {
        const auto& node = element(list, index);
        switch (node.type) {
        case NodeType::weak_blocker:
        case NodeType::strong_blocker:
            return true;
        case NodeType::atom: {
            if (satisfied_atom(tables, node)) {
                return true;
            }
            const auto text = tables.string(node.atom);
            const auto& wanted = atom(text);
            const auto found = wanted ? pullable(*wanted) : std::nullopt;
            if (!found) {
                return false;
            }
            place(*found);
            pulls.emplace_back(*found, std::string(text));
            return true;
        }
        case NodeType::any_of:
        case NodeType::all_of: {
            const bool any = node.type == NodeType::any_of;
            const auto start = pulls.size();
            bool some = false;
            for (std::size_t child = index + 1; child < list.size(); ++child) {
                if (element(list, child).parent != index) {
                    continue;
                }
                some = true;
                const auto before = pulls.size();
                const bool done = pull(tables, list, child, pulls);
                if (any && done) {
                    return true;
                }
                if (!any && !done) {
                    rollback(pulls, start);
                    return false;
                }
                if (!done) {
                    rollback(pulls, before);
                }
            }
            return !any || !some;
        }
        }
        return false;
    }

    void rollback(std::vector<std::pair<std::uint32_t, std::string>>& pulls, std::size_t size) {
        while (pulls.size() > size) {
            unplace(pulls.back().first);
            pulls.pop_back();
        }
    }

    // The first atom under node index that an installed package being replaced matches, and
    // that package.
    std::optional<std::pair<std::uint32_t, std::string>>
    replaced_match(const Tables& tables, std::span<const Node> list, std::size_t index) {
        for (std::size_t i = index; i < list.size(); ++i) {
            if (i != index && !descends(list, i, index)) {
                continue;
            }
            const auto& node = element(list, i);
            if (node.type != NodeType::atom) {
                continue;
            }
            for (const auto id : tables.ids_in(node.matches)) {
                if (!kept(id)) {
                    return std::pair{id, std::string(tables.string(node.atom))};
                }
            }
        }
        return std::nullopt;
    }

    static bool descends(std::span<const Node> list, std::size_t node, std::size_t ancestor) {
        for (auto at = element(list, node).parent; at != no_parent; at = element(list, at).parent) {
            if (at == ancestor) {
                return true;
            }
        }
        return false;
    }

    // The first alternative of a || under node index that an installed package being replaced
    // matches, and that package, when no argument names it: emerge's || prefers what is
    // installed, and only an argument updates it regardless.
    std::optional<std::pair<std::uint32_t, std::string>>
    installed_alternative(const Tables& tables, std::span<const Node> list, std::size_t index) {
        for (std::size_t i = index; i < list.size(); ++i) {
            const auto& node = element(list, i);
            if (node.type != NodeType::atom || node.parent == no_parent ||
                element(list, node.parent).type != NodeType::any_of ||
                (i != index && !descends(list, i, index))) {
                continue;
            }
            for (const auto id : tables.ids_in(node.matches)) {
                if (!kept(id) && !argument(id)) {
                    return std::pair{id, std::string(tables.string(node.atom))};
                }
            }
        }
        return std::nullopt;
    }

    // One pass over the current choices: false once none of them moved.
    bool settle() {
        pulled_.clear();
        present_.clear();
        taken_.clear();
        rebuilt_.clear();
        std::vector<Work> work;
        for (std::uint32_t id = 0; id < choices_.size(); ++id) {
            if (const auto merged = choices_.at(id).merged()) {
                place(*merged);
                work.push_back({.member = {.candidate = true, .index = *merged}, .root = id});
            }
        }
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            if (kept(id) && in_scope(id)) {
                work.push_back({.member = {.candidate = false, .index = id}, .root = {}});
            }
        }
        std::map<std::uint32_t, std::vector<Reason>> rejected;
        for (std::size_t w = 0; w < work.size(); ++w) {
            const auto item = work.at(w);
            if (item.root && rejected.contains(*item.root)) {
                continue;
            }
            if (!item.member.candidate) {
                // A rebuild's own dependencies stand in for the installed ones.
                if (const auto broken = broken_bindings(item.member.index); !broken.empty()) {
                    if (const auto own = rebuild_of(item.member.index)) {
                        const auto& first = broken.front();
                        rebuilt_.emplace(
                            item.member.index,
                            Rebuilt{.candidate = *own,
                                    .why = {.member = {.candidate = true, .index = first.merged},
                                            .atom = first.atom}});
                        work.push_back(
                            {.member = {.candidate = true, .index = *own}, .root = first.replaced});
                    } else {
                        for (const auto& each : broken) {
                            rejected[each.replaced].push_back(
                                {.member = item.member, .atom = each.atom});
                        }
                    }
                    continue;
                }
                if (const auto moved = new_slots_.find(item.member.index);
                    moved != new_slots_.end()) {
                    if (const auto child = moved_to(item.member.index, moved->second, work)) {
                        const auto& found = moved->second;
                        rebuilt_.emplace(item.member.index,
                                         Rebuilt{.candidate = found.own,
                                                 .why = {.member = *child, .atom = found.bound}});
                        work.push_back({.member = {.candidate = true, .index = found.own},
                                        .root = child->candidate ? found.installed : std::nullopt});
                        continue;
                    }
                }
            }
            const auto& tables = this->tables(item.member);
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                const auto list = nodes(item.member, kind);
                const auto ok = satisfied_nodes(tables, list);
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (element(list, i).parent != no_parent || ok.at(i)) {
                        continue;
                    }
                    if (const auto held = installed_alternative(tables, list, i)) {
                        rejected[held->first].push_back(
                            {.member = item.member, .atom = held->second});
                        continue;
                    }
                    std::vector<std::pair<std::uint32_t, std::string>> pulls;
                    if (pull(tables, list, i, pulls)) {
                        for (auto& [candidate, text] : pulls) {
                            pulled_.push_back(
                                {.candidate = candidate,
                                 .by = {.member = item.member, .atom = std::move(text)},
                                 .root = item.root});
                            work.push_back({.member = {.candidate = true, .index = candidate},
                                            .root = item.root});
                        }
                        continue;
                    }
                    if (const auto held = replaced_match(tables, list, i)) {
                        rejected[held->first].push_back(
                            {.member = item.member, .atom = held->second});
                    } else if (item.root) {
                        rejected[*item.root].push_back(
                            {.member = item.member, .atom = first_atom(tables, list, i)});
                    }
                }
            }
        }
        for (auto& [id, reasons] : rejected) {
            auto& choice = choices_.at(id);
            if (choice.at == 0) {
                std::ranges::sort(reasons);
                const auto [first, last] = std::ranges::unique(reasons);
                reasons.erase(first, last);
                choice.reasons = std::move(reasons);
            }
            ++choice.at;
        }
        return !rejected.empty();
    }

    static std::string first_atom(const Tables& tables, std::span<const Node> list,
                                  std::size_t index) {
        for (std::size_t i = index; i < list.size(); ++i) {
            const auto& node = element(list, i);
            if (node.type == NodeType::atom && (i == index || descends(list, i, index))) {
                return std::string(tables.string(node.atom));
            }
        }
        return {};
    }

    [[nodiscard]] Plan result() const {
        Plan plan;
        for (std::uint32_t id = 0; id < choices_.size(); ++id) {
            if (const auto found = rebuilt_.find(id); found != rebuilt_.end()) {
                plan.merges.push_back({.candidate = found->second.candidate,
                                       .replaces = id,
                                       .kind = UpdateKind::rebuild,
                                       .flags = {},
                                       .pulled_by = {},
                                       .rebuilt_for = found->second.why,
                                       .waits = {}});
            }
            const auto& choice = choices_.at(id);
            if (!choice.wanted) {
                continue;
            }
            if (const auto merged = choice.merged()) {
                Merge merge{.candidate = *merged,
                            .replaces = id,
                            .kind = UpdateKind::upgrade,
                            .flags = {},
                            .pulled_by = {},
                            .rebuilt_for = {},
                            .waits = {}};
                if (choice.at == 0) {
                    merge.kind = choice.wanted->kind;
                    merge.flags = choice.wanted->flags;
                } else {
                    merge.kind =
                        update_kind(evaluated().string(evaluated().candidates.at(*merged).cp),
                                    store().string(store().packages.at(id).cpv),
                                    evaluated().string(evaluated().candidates.at(*merged).cpv));
                }
                plan.merges.push_back(std::move(merge));
            }
            if (choice.at != 0 && !choice.induced) {
                plan.held.push_back(
                    {.package = id, .wanted = *choice.wanted, .reasons = choice.reasons});
            }
        }
        auto pulled = pulled_;
        std::ranges::sort(pulled, [this](const Pulled& a, const Pulled& b) {
            return evaluated().string(evaluated().candidates.at(a.candidate).cpv) <
                   evaluated().string(evaluated().candidates.at(b.candidate).cpv);
        });
        for (const auto& found : pulled) {
            plan.merges.push_back({.candidate = found.candidate,
                                   .replaces = {},
                                   .kind = UpdateKind::upgrade,
                                   .flags = {},
                                   .pulled_by = found.by,
                                   .rebuilt_for = {},
                                   .waits = {}});
        }
        return plan;
    }
};

// Sets plan.order and each merge's waits.
void order_merges(const Store& store, const Evaluated& evaluated, Plan& plan) {
    const auto count = static_cast<std::uint32_t>(plan.merges.size());
    const auto candidate = [&](std::uint32_t merge) -> const Candidate& {
        return evaluated.candidates.at(plan.merges.at(merge).candidate);
    };
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp;
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        by_cp[std::string(evaluated.string(candidate(merge).cp))].push_back(merge);
    }
    std::map<std::string, std::optional<Atom>, std::less<>> atoms;
    // The merges each one waits for, true where only a run-time dependency does.
    std::vector<std::map<std::uint32_t, bool>> needs(count);
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            if (dep_kinds.at(kind) == "PDEPEND") {
                continue;
            }
            const bool runtime = dep_kinds.at(kind) == "RDEPEND";
            for (const auto& node : evaluated.nodes_in(candidate(merge).deps.at(kind))) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                const auto text = evaluated.string(node.atom);
                auto parsed = atoms.find(text);
                if (parsed == atoms.end()) {
                    auto atom = parse_atom(text);
                    if (atom && atom->slot_operator) {
                        atom->sub_slot.reset();
                    }
                    parsed = atoms
                                 .emplace(std::string(text),
                                          atom ? std::optional{std::move(*atom)} : std::nullopt)
                                 .first;
                }
                const auto& atom = parsed->second;
                if (!atom) {
                    continue;
                }
                const auto same = by_cp.find(atom->cp);
                if (same == by_cp.end()) {
                    continue;
                }
                for (const auto other : same->second) {
                    if (other == merge || !matches(store, evaluated, candidate(other), *atom)) {
                        continue;
                    }
                    const auto [found, added] = needs.at(merge).emplace(other, runtime);
                    if (!added) {
                        found->second = found->second && runtime;
                    }
                }
            }
        }
    }
    // emerge has every merge wait for libc (virtual/libc's provider): it goes first, with what
    // it waits for, though the waits are not listed.
    std::vector<bool> early(count);
    std::vector<std::uint32_t> stack;
    for (const auto& pkg : store.packages) {
        if (store.string(pkg.cp) != "virtual/libc") {
            continue;
        }
        for (const auto range : pkg.deps) {
            for (const auto& node : store.nodes_in(range)) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                for (const auto id : store.ids_in(node.matches)) {
                    const auto same = by_cp.find(store.string(store.packages.at(id).cp));
                    if (same != by_cp.end()) {
                        std::ranges::copy(same->second, std::back_inserter(stack));
                    }
                }
            }
        }
    }
    while (!stack.empty()) {
        const auto merge = stack.back();
        stack.pop_back();
        if (!early.at(merge)) {
            early.at(merge) = true;
            for (const auto& [other, runtime] : needs.at(merge)) {
                stack.push_back(other);
            }
        }
    }
    std::vector<bool> placed(count);
    while (plan.order.size() < count) {
        // Ready first, then ready but for run-time waits, then the fewest waits left; libc
        // first among them, then plan order.
        std::optional<std::tuple<int, bool, std::size_t, std::size_t, std::uint32_t>> best;
        for (std::uint32_t merge = 0; merge < count; ++merge) {
            if (placed.at(merge)) {
                continue;
            }
            std::size_t build = 0;
            std::size_t all = 0;
            for (const auto& [other, runtime] : needs.at(merge)) {
                if (!placed.at(other)) {
                    ++all;
                    build += runtime ? 0 : 1;
                }
            }
            const std::tuple key{all == 0     ? 0
                                 : build == 0 ? 1
                                              : 2,
                                 !early.at(merge), build, all, merge};
            if (!best || key < *best) {
                best = key;
            }
        }
        if (!best) {
            break;
        }
        const auto merge = std::get<4>(*best);
        for (const auto& [other, runtime] : needs.at(merge)) {
            if (placed.at(other)) {
                plan.merges.at(merge).waits.push_back(other);
            }
        }
        placed.at(merge) = true;
        plan.order.push_back(merge);
    }
}

} // namespace

Plan plan_updates(const Store& store, const Evaluated& evaluated, UseRebuilds rebuilds,
                  const Targets& targets) {
    auto plan = Planner(store, evaluated, rebuilds, targets).run();
    order_merges(store, evaluated, plan);
    return plan;
}

} // namespace egraph
