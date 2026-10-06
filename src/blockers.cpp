#include "blockers.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace egraph {

namespace {

bool is_blocker(const Node& node) {
    return node.type == NodeType::weak_blocker || node.type == NodeType::strong_blocker;
}

const Node& element(std::span<const Node> nodes, std::size_t index) {
    return nodes.subspan(index, 1).front();
}

// The kinds emerge reads an installed package's blockers from: only what it needs at run time.
bool runtime_kind(std::size_t kind) {
    const auto name = dep_kinds.at(kind);
    return name == "IDEPEND" || name == "PDEPEND" || name == "RDEPEND";
}

class Weigher {
  public:
    Weigher(const Store& store EGRAPH_KEPT_BY_THIS, const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
            const Targets& targets EGRAPH_KEPT_BY_THIS, const Plan& plan)
        : store_ref_(store), evaluated_ref_(evaluated), targets_ref_(targets),
          kept_(store.packages.size(), true) {
        for (const auto& merge : plan.merges) {
            merged_.push_back(merge.candidate);
            by_cp_[std::string(evaluated.string(evaluated.candidates.at(merge.candidate).cp))]
                .push_back(merge.candidate);
            if (merge.replaces) {
                kept_.at(*merge.replaces) = false;
                replacing_.emplace(*merge.replaces, merge.candidate);
            }
        }
        complete();
    }

    // The masked installed packages emerge warns of (_masked_installed): kept, not visible, and
    // in the completed graph or masked by LICENSE.
    [[nodiscard]] std::vector<std::uint32_t> masked() const {
        std::vector<std::uint32_t> found;
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            const auto& pkg = evaluated().packages.at(id);
            const auto hidden = targets_ref_.get().dynamic_deps ? pkg.hidden : pkg.vdb_hidden;
            if (kept_.at(id) && (hidden == Hidden::license ||
                                 (hidden == Hidden::hidden &&
                                  needed_.contains({.candidate = false, .index = id})))) {
                found.push_back(id);
            }
        }
        return found;
    }

    void run(Plan& plan) {
        // By installed package: the first blocker needing it gone, and the merges in its way.
        std::map<std::uint32_t, std::pair<Block, std::set<std::uint32_t>>> uninstalls;
        std::set<Block> blocks;
        const auto weigh = [&](const Member& holder, std::size_t kind) {
            for (const auto& node : nodes(holder, kind)) {
                if (node.parent == no_parent && is_blocker(node)) {
                    weigh_one(holder, node, uninstalls, blocks);
                }
            }
        };
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                if (kept_.at(id) && runtime_kind(kind)) {
                    weigh({.candidate = false, .index = id}, kind);
                }
            }
        }
        for (const auto candidate : merged_) {
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                weigh({.candidate = true, .index = candidate}, kind);
            }
        }
        std::map<std::uint32_t, std::uint32_t> by_candidate;
        for (std::uint32_t i = 0; i < plan.merges.size(); ++i) {
            by_candidate.emplace(plan.merges.at(i).candidate, i);
        }
        for (auto& [id, found] : uninstalls) {
            auto& [why, merges] = found;
            std::vector<std::uint32_t> after;
            for (const auto candidate : merges) {
                after.push_back(by_candidate.at(candidate));
            }
            std::ranges::sort(after);
            plan.uninstalls.push_back({.package = id, .why = std::move(why), .after = after});
        }
        plan.blocks.assign(blocks.begin(), blocks.end());
    }

  private:
    std::reference_wrapper<const Store> store_ref_;
    std::reference_wrapper<const Evaluated> evaluated_ref_;
    std::reference_wrapper<const Targets> targets_ref_;
    // Per installed package, whether it stays: no merge replaces it.
    std::vector<bool> kept_;
    std::vector<std::uint32_t> merged_;
    // Installed packages a merge replaces, with the merge's candidate.
    std::map<std::uint32_t, std::uint32_t> replacing_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp_;
    std::map<std::string, std::optional<Atom>, std::less<>> atoms_;
    // What emerge's completed graph holds.
    std::set<Member> needed_;

    [[nodiscard]] const Store& store() const { return store_ref_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_ref_.get(); }

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

    [[nodiscard]] std::string_view cp(const Member& member) const {
        return member.candidate ? evaluated().string(evaluated().candidates.at(member.index).cp)
                                : store().string(store().packages.at(member.index).cp);
    }

    [[nodiscard]] std::string_view cpv(const Member& member) const {
        return member.candidate ? evaluated().string(evaluated().candidates.at(member.index).cpv)
                                : store().string(store().packages.at(member.index).cpv);
    }

    [[nodiscard]] bool same_slot(const Member& a, const Member& b) const {
        const auto slot = [this](const Member& member) {
            return member.candidate
                       ? evaluated().string(evaluated().candidates.at(member.index).slot)
                       : store().string(store().packages.at(member.index).slot);
        };
        return cp(a) == cp(b) && slot(a) == slot(b);
    }

    // The atom of a dependency or blocker, without its "!"s; a slot operator's sub-slot is only
    // what the package was built against, not a bound.
    const std::optional<Atom>& atom(std::string_view text) {
        auto found = atoms_.find(text);
        if (found == atoms_.end()) {
            auto parsed =
                parse_atom(text.substr(std::min(text.find_first_not_of('!'), text.size())));
            std::optional<Atom> value;
            if (parsed) {
                if (parsed->slot_operator) {
                    parsed->sub_slot.reset();
                }
                value = std::move(*parsed);
            }
            found = atoms_.emplace(std::string(text), std::move(value)).first;
        }
        return found->second;
    }

    // The merges an atom matches.
    std::vector<std::uint32_t> merged_matches(const std::optional<Atom>& wanted) {
        std::vector<std::uint32_t> found;
        if (!wanted) {
            return found;
        }
        if (const auto same = by_cp_.find(wanted->cp); same != by_cp_.end()) {
            for (const auto candidate : same->second) {
                if (matches(store(), evaluated(), evaluated().candidates.at(candidate), *wanted)) {
                    found.push_back(candidate);
                }
            }
        }
        return found;
    }

    // The best version left installed that an atom matches, of the installed packages it
    // matches (ids) and the merges.
    std::optional<Member> select(std::span<const std::uint32_t> ids,
                                 const std::optional<Atom>& wanted) {
        std::optional<Member> best;
        std::optional<Version> best_version;
        const auto consider = [&](const Member& member) {
            const auto text = cpv(member);
            auto version = parse_version(text.substr(std::min(text.size(), cp(member).size() + 1)));
            if (!best || (version && best_version && vercmp(*version, *best_version) > 0)) {
                best = member;
                best_version = std::move(version);
            }
        };
        for (const auto id : ids) {
            if (kept_.at(id)) {
                consider({.candidate = false, .index = id});
            }
        }
        for (const auto candidate : merged_matches(wanted)) {
            consider({.candidate = true, .index = candidate});
        }
        return best;
    }

    std::optional<Member> select(const Tables& tables, const Node& node) {
        return select(tables.ids_in(node.matches), atom(tables.string(node.atom)));
    }

    bool satisfied(const Tables& tables, std::span<const Node> list, std::size_t index) {
        const auto& node = element(list, index);
        switch (node.type) {
        case NodeType::weak_blocker:
        case NodeType::strong_blocker:
            return true;
        case NodeType::atom:
            return select(tables, node).has_value();
        case NodeType::any_of:
        case NodeType::all_of: {
            const bool any = node.type == NodeType::any_of;
            bool some = false;
            for (std::size_t child = index + 1; child < list.size(); ++child) {
                if (element(list, child).parent != index) {
                    continue;
                }
                some = true;
                if (satisfied(tables, list, child) == any) {
                    return any;
                }
            }
            return !any || !some;
        }
        }
        return false;
    }

    // What the node selects joins the completed graph: an atom's best match, a group's
    // members, a ||'s first alternative left satisfied.
    void take(const Tables& tables, std::span<const Node> list, std::size_t index,
              std::vector<Member>& queue) {
        const auto& node = element(list, index);
        if (node.type == NodeType::atom) {
            if (const auto found = select(tables, node)) {
                reach(*found, queue);
            }
            return;
        }
        if (is_blocker(node)) {
            return;
        }
        for (std::size_t child = index + 1; child < list.size(); ++child) {
            if (element(list, child).parent != index) {
                continue;
            }
            if (node.type == NodeType::all_of) {
                take(tables, list, child, queue);
            } else if (satisfied(tables, list, child)) {
                take(tables, list, child, queue);
                return;
            }
        }
    }

    void reach(const Member& member, std::vector<Member>& queue) {
        if (needed_.insert(member).second) {
            queue.push_back(member);
        }
    }

    // emerge completes its graph before uninstalling anything: the root sets, its arguments and
    // the merges, and what their dependencies select, deep.
    void complete() {
        std::vector<Member> queue;
        for (const auto& root : store().roots) {
            if (const auto found =
                    select(store().ids_in(root.matches), atom(store().string(root.atom)))) {
                reach(*found, queue);
            }
        }
        const auto& targets = targets_ref_.get();
        if (!targets.roots) {
            for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
                if (kept_.at(id)) {
                    reach({.candidate = false, .index = id}, queue);
                }
            }
        }
        for (const auto& argument : targets.request) {
            const auto& wanted = atom(argument.atom);
            std::vector<std::uint32_t> ids;
            for (std::uint32_t id = 0; wanted && id < store().packages.size(); ++id) {
                if (matches(store(), store().packages.at(id), *wanted)) {
                    ids.push_back(id);
                }
            }
            if (const auto found = select(ids, wanted)) {
                reach(*found, queue);
            }
        }
        for (const auto candidate : merged_) {
            reach({.candidate = true, .index = candidate}, queue);
        }
        while (!queue.empty()) {
            const auto member = queue.back();
            queue.pop_back();
            const auto& tables = this->tables(member);
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                const auto list = nodes(member, kind);
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (element(list, i).parent == no_parent) {
                        take(tables, list, i, queue);
                    }
                }
            }
        }
    }

    // One blocker of holder, as _validate_blockers weighs it.
    void weigh_one(const Member& holder, const Node& node,
                   std::map<std::uint32_t, std::pair<Block, std::set<std::uint32_t>>>& uninstalls,
                   std::set<Block>& blocks) {
        const auto& tables = this->tables(holder);
        const auto text = std::string(tables.string(node.atom));
        const bool strong = node.type == NodeType::strong_blocker;
        const bool running = targets_ref_.get().running_root;
        bool unresolved = false;
        std::vector<Member> blocked;
        std::vector<std::pair<std::uint32_t, Member>> removals;
        const auto removable = [&](std::uint32_t id) {
            return !needed_.contains({.candidate = false, .index = id}) && !(strong && running);
        };
        const auto ignored = [&](const Member& other) {
            return same_slot(holder, other) && !strong;
        };
        if (!holder.candidate) {
            // An installed holder: only a merge it blocks matters, and the holder must go.
            for (const auto candidate : merged_matches(atom(text))) {
                const Member other{.candidate = true, .index = candidate};
                if (ignored(other)) {
                    continue;
                }
                blocked.push_back(other);
                if (removable(holder.index)) {
                    removals.emplace_back(holder.index, other);
                } else {
                    unresolved = true;
                }
            }
        } else {
            for (const auto id : tables.ids_in(node.matches)) {
                const Member other{.candidate = false, .index = id};
                if (ignored(other)) {
                    continue;
                }
                blocked.push_back(other);
                if (kept_.at(id)) {
                    if (removable(id)) {
                        removals.emplace_back(id, other);
                    } else {
                        unresolved = true;
                    }
                } else if (strong && running && replacing_.at(id) == holder.index) {
                    // The merge waits on the uninstall only its own replacement would do.
                    unresolved = true;
                }
            }
            for (const auto candidate : merged_matches(atom(text))) {
                const Member other{.candidate = true, .index = candidate};
                if (!ignored(other)) {
                    blocked.push_back(other);
                    unresolved = true;
                }
            }
        }
        if (unresolved) {
            for (const auto& other : blocked) {
                blocks.insert({.holder = holder, .atom = text, .blocked = other});
            }
            return;
        }
        for (const auto& [id, other] : removals) {
            auto& found =
                uninstalls
                    .try_emplace(id, Block{.holder = holder, .atom = text, .blocked = other},
                                 std::set<std::uint32_t>{})
                    .first->second;
            found.second.insert(holder.candidate ? holder.index : other.index);
        }
    }
};

} // namespace

std::vector<std::string> blocker_lines(const Store& store, std::span<const std::uint32_t> named) {
    const auto cpv = [&store](std::uint32_t id) { return store.string(store.packages.at(id).cpv); };
    std::vector<std::string> lines;
    for (std::uint32_t holder = 0; holder < store.packages.size(); ++holder) {
        const bool holds = std::ranges::contains(named, holder);
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            for (const auto& node : store.nodes_in(store.packages.at(holder).deps.at(kind))) {
                if (!is_blocker(node)) {
                    continue;
                }
                const auto line = [&](std::string_view blocked) {
                    lines.push_back(std::format("{}\t{}\t{}\t{}", cpv(holder), dep_kinds.at(kind),
                                                store.string(node.atom), blocked));
                };
                const auto blocked = store.ids_in(node.matches);
                if (holds && blocked.empty()) {
                    line("");
                }
                for (const auto id : blocked) {
                    if (named.empty() || holds || std::ranges::contains(named, id)) {
                        line(cpv(id));
                    }
                }
            }
        }
    }
    std::ranges::sort(lines);
    const auto [first, last] = std::ranges::unique(lines);
    lines.erase(first, last);
    return lines;
}

void weigh_blockers(const Store& store, const Evaluated& evaluated, const Targets& targets,
                    Plan& plan) {
    Weigher weigher(store, evaluated, targets, plan);
    weigher.run(plan);
    plan.masked = weigher.masked();
}

} // namespace egraph
