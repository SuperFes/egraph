#include "plan.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace egraph {

namespace {

const Node& element(std::span<const Node> nodes, std::size_t index) {
    return nodes.subspan(index, 1).front();
}

// Where a slot is taken: by cp and slot, both as the tables spell them.
using SlotKey = std::pair<std::string, std::string>;

class Planner {
  public:
    Planner(const Store& store EGRAPH_KEPT_BY_THIS, const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
            UseRebuilds rebuilds, const std::vector<bool>& scope EGRAPH_KEPT_BY_THIS)
        : store_ref_(store), evaluated_ref_(evaluated), scope_ref_(scope),
          choices_(store.packages.size()) {
        for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
            by_cp_[std::string(evaluated.string(evaluated.candidates.at(i).cp))].push_back(i);
        }
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (auto wanted = pending_update(evaluated, id, rebuilds)) {
                auto& choice = choices_.at(id);
                choice.options.push_back(wanted->target);
                std::ranges::copy(fallbacks(evaluated, id, *wanted),
                                  std::back_inserter(choice.options));
                choice.wanted = std::move(wanted);
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

    // A member whose dependencies the plan must satisfy.
    struct Work {
        Member member;
        std::optional<std::uint32_t> root;
    };

    std::reference_wrapper<const Store> store_ref_;
    std::reference_wrapper<const Evaluated> evaluated_ref_;
    std::reference_wrapper<const std::vector<bool>> scope_ref_;
    std::vector<Choice> choices_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp_;
    std::map<std::string, std::optional<Atom>, std::less<>> atoms_;

    // The state of one pass, rebuilt from the choices.
    std::vector<Pulled> pulled_;
    // Merged and pulled candidates by cp.
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> present_;
    std::map<SlotKey, bool> taken_;

    [[nodiscard]] const Store& store() const { return store_ref_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_ref_.get(); }

    [[nodiscard]] bool in_scope(std::uint32_t id) const {
        const auto& scope = scope_ref_.get();
        return scope.empty() || scope.at(id);
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
            auto version =
                parse_version(evaluated()
                                  .string(candidate.cpv)
                                  .substr(std::min(evaluated().string(candidate.cpv).size(),
                                                   wanted.cp.size() + 1)));
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

    // One pass over the current choices: false once none of them moved.
    bool settle() {
        pulled_.clear();
        present_.clear();
        taken_.clear();
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
            const auto& tables = this->tables(item.member);
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                const auto list = nodes(item.member, kind);
                const auto ok = satisfied_nodes(tables, list);
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (element(list, i).parent != no_parent || ok.at(i)) {
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
            const auto& choice = choices_.at(id);
            if (!choice.wanted) {
                continue;
            }
            if (const auto merged = choice.merged()) {
                Merge merge{.candidate = *merged,
                            .replaces = id,
                            .kind = UpdateKind::upgrade,
                            .flags = {},
                            .pulled_by = {}};
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
            if (choice.at != 0) {
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
                                   .pulled_by = found.by});
        }
        return plan;
    }
};

} // namespace

Plan plan_updates(const Store& store, const Evaluated& evaluated, UseRebuilds rebuilds,
                  const std::vector<bool>& scope) {
    return Planner(store, evaluated, rebuilds, scope).run();
}

} // namespace egraph
