#include "plan.hpp"

#include "atom.hpp"
#include "blockers.hpp"
#include "graph.hpp"
#include "replace.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
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

// Where the dependency kind comes in emerge's walk of a package's dependencies.
std::size_t emerge_rank(std::size_t kind) {
    constexpr std::array<std::string_view, 5> order{"RDEPEND", "IDEPEND", "PDEPEND", "DEPEND",
                                                    "BDEPEND"};
    return static_cast<std::size_t>(std::ranges::find(order, dep_kinds.at(kind)) - order.begin());
}

class Planner {
  public:
    Planner(const Store& store EGRAPH_KEPT_BY_THIS, const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
            UseRebuilds rebuilds, const Targets& targets EGRAPH_KEPT_BY_THIS,
            std::span<const UseChange> changes = {})
        : store_ref_(store), evaluated_ref_(evaluated), targets_ref_(targets),
          choices_(store.packages.size()) {
        for (const auto& change : changes) {
            changed_.emplace(change.candidate, change.flags);
        }
        for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
            by_cp_[std::string(evaluated.string(evaluated.candidates.at(i).cp))].push_back(i);
        }
        if (targets.roots) {
            collect_arguments();
            lenient_ = std::ranges::all_of(args_, [](const Arg& arg) {
                return arg.named.set == "selected" || arg.named.set == "system" ||
                       arg.named.set == "profile";
            });
            arguments_.assign(store.packages.size(), false);
            alone_.assign(store.packages.size(), false);
            must_.assign(store.packages.size(), false);
            named_.assign(store.packages.size(), false);
            for (std::uint32_t i = 0; i < args_.size(); ++i) {
                select(i);
            }
        }
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (!in_scope(id) || (targets.deep ? !reached(id) : !argument(id)) ||
                dropped_.contains(id)) {
                continue;
            }
            auto wanted = pending_update(evaluated, id, rebuilds);
            const auto forced = forced_.find(id);
            if (forced != forced_.end()) {
                wanted = forced_update(id, forced->second, std::move(wanted));
            }
            if (wanted) {
                auto& choice = choices_.at(id);
                choice.options.push_back(wanted->target);
                // emerge must merge what it is asked for, so it backtracks to another version.
                const auto& atom =
                    forced == forced_.end() ? std::nullopt : args_.at(forced->second.argument).atom;
                if (atom && must(id)) {
                    std::ranges::copy(other_matches(wanted->target, *atom),
                                      std::back_inserter(choice.options));
                } else if (targets.deep && !alone(id)) {
                    // Only the deep resolution looks further; plain -u drops a rejected update,
                    // as -uD does an atom's named alone.
                    for (const auto fallback : fallbacks(evaluated, id, *wanted)) {
                        if (!atom ||
                            matches(store, evaluated, evaluated.candidates.at(fallback), *atom)) {
                            choice.options.push_back(fallback);
                        }
                    }
                }
                choice.wanted = std::move(wanted);
            }
        }
        // Only the deep resolution probes an installed dependent for a newer slot.
        if (targets.roots && targets.deep) {
            for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
                if (in_scope(id) && reached(id) && !choices_.at(id).wanted) {
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

    // The USE changes run() found needed beyond those made already.
    [[nodiscard]] std::vector<NeededUseChange> proposed() && { return std::move(proposed_); }

  private:
    struct Choice {
        std::optional<PendingUpdate> wanted;
        // The target, then its fallbacks.
        std::vector<std::uint32_t> options;
        // Index into options; options.size() keeps the installed package.
        std::size_t at = 0;
        // What rejected the target.
        std::vector<Reason> reasons;
        // Some of them nothing can satisfy.
        bool unsatisfiable = false;
        // Wanted only for a dependent's move to a newer slot, or for a merge that needs it, so
        // never held.
        bool induced = false;

        [[nodiscard]] std::optional<std::uint32_t> merged() const {
            return at < options.size() ? std::optional{options.at(at)} : std::nullopt;
        }
    };

    // One of emerge's arguments, with the installed packages its atom matches.
    struct Arg {
        Argument named;
        std::optional<Atom> atom;
        std::vector<std::uint32_t> matches;
    };

    // An installed package whose target an argument's atom decides: its best version, or none
    // to keep it.
    struct Forced {
        std::optional<std::uint32_t> candidate;
        // Index into args_.
        std::uint32_t argument = 0;
    };

    struct Pulled {
        std::uint32_t candidate = 0;
        std::optional<Reason> by;
        // Index into args_, for one an argument names.
        std::optional<std::uint32_t> named_by;
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
    std::vector<Arg> args_;
    // Installed packages that emerge's arguments name; every one when empty.
    std::vector<bool> arguments_;
    // Those -u updates for an atom named alone rather than in a set: emerge keeps the installed
    // version rather than fall back or rebuild what binds to it, even with --deep.
    std::vector<bool> alone_;
    // Those an argument must merge: emerge backtracks to another version it matches and
    // rebuilds what binds to it, as it would for a package it must pull in.
    std::vector<bool> must_;
    // Those an atom named alone, rather than a set, must merge: with no version left, emerge
    // refuses rather than keep the installed one.
    std::vector<bool> named_;
    // Only the world sets are arguments: -uD skips what they reach and keep whose dependency
    // nothing satisfies, or only what is installed in a slot-operator binding's slot, rather
    // than refuse.
    bool lenient_ = false;
    std::map<std::uint32_t, Forced> forced_;
    // Installed slots greedy slots leave out, blocked by the atom's best version or blocking
    // it: emerge neither updates them nor takes them as arguments.
    std::set<std::uint32_t> dropped_;
    std::map<std::uint32_t, NewSlot> new_slots_;
    // Arguments (indices into args_) whose best visible match is new in its slot, with that
    // candidate.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> root_pulls_;
    // Arguments (indices into args_) no version of which could be merged.
    std::set<std::uint32_t> refused_;
    // Arguments plain emerge has no visible version of to merge.
    std::set<std::uint32_t> unmatched_;
    // Candidates emerge's backtracking masks once a dependency of theirs failed, with it.
    std::map<std::uint32_t, Reason> backtracked_;
    // Candidates emerge would have selected in some pass, and so checked their REQUIRED_USE.
    std::set<std::uint32_t> selected_;
    // The USE changes evaluated already has made, by candidate.
    std::map<std::uint32_t, std::map<std::string, bool, std::less<>>> changed_;
    // Further USE changes autounmask asks for, each candidate's first; and those a pull asked
    // for before it was known whether it fails.
    std::vector<NeededUseChange> proposed_;
    std::vector<NeededUseChange> pending_;
    // The member whose dependencies are being pulled in.
    std::optional<Member> puller_;
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
    // Installed packages a merge replaces that breaks a binding within -uD's reach.
    std::set<std::uint32_t> triggers_;
    // Installed packages a pull found in the way, with the candidate it wanted; and those a
    // failed dependency needs, given their update once the pass is over.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> wanted_;
    std::map<std::uint32_t, std::uint32_t> needed_;
    // Dependencies of kept packages that nothing satisfies and that emerge must.
    std::vector<Reason> missing_;

    [[nodiscard]] const Store& store() const { return store_ref_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_ref_.get(); }

    [[nodiscard]] bool in_scope(std::uint32_t id) const {
        const auto& scope = targets_ref_.get().scope;
        return scope.empty() || scope.at(id);
    }

    [[nodiscard]] bool reached(std::uint32_t id) const {
        const auto& reach = targets_ref_.get().reach;
        return reach.empty() || reach.at(id);
    }

    [[nodiscard]] bool alone(std::uint32_t id) const { return !alone_.empty() && alone_.at(id); }

    [[nodiscard]] bool must(std::uint32_t id) const { return !must_.empty() && must_.at(id); }

    [[nodiscard]] bool named(std::uint32_t id) const { return !named_.empty() && named_.at(id); }

    [[nodiscard]] bool argument(std::uint32_t id) const {
        return arguments_.empty() || arguments_.at(id);
    }

    [[nodiscard]] bool deep() const { return targets_ref_.get().deep; }

    void collect_arguments() {
        const auto& request = targets_ref_.get().request;
        if (request.empty()) {
            for (const auto& root : store().roots) {
                const auto ids = store().ids_in(root.matches);
                args_.push_back({.named = {.set = std::string(store().string(root.set)),
                                           .atom = std::string(store().string(root.atom))},
                                 .atom = atom(store().string(root.atom)),
                                 .matches = {ids.begin(), ids.end()}});
            }
            return;
        }
        for (const auto& named : request) {
            Arg arg{.named = named, .atom = atom(named.atom), .matches = {}};
            for (std::uint32_t id = 0; arg.atom && id < store().packages.size(); ++id) {
                if (matches(store(), store().packages.at(id), *arg.atom)) {
                    arg.matches.push_back(id);
                }
            }
            args_.push_back(std::move(arg));
        }
    }

    // What argument i updates, merges or pulls in, as its selection picks.
    void select(std::uint32_t i) {
        const auto& arg = args_.at(i);
        const auto selection = targets_ref_.get().selection;
        const auto best = arg.atom ? best_match(*arg.atom) : std::nullopt;
        if (selection == Selection::update) {
            // Every installed slot the atom matches, as emerge's greedy slots; a set's atom only
            // its best version's slot, as emerge takes greedy slots for atom arguments alone.
            // The others stay unless a dependency reaches them.
            for (const auto id : arg.matches) {
                if (best && !arg.named.set.empty() &&
                    store().string(store().packages.at(id).slot) !=
                        evaluated().string(evaluated().candidates.at(*best).slot)) {
                    continue;
                }
                if (best && greedy_blocked(id, *best)) {
                    dropped_.insert(id);
                    continue;
                }
                arguments_.at(id) = true;
                alone_.at(id) = alone_.at(id) || arg.named.set.empty();
                // With no visible version to move to, only an update the atom accepts.
                if (!best) {
                    forced_.try_emplace(id, Forced{.candidate = std::nullopt, .argument = i});
                }
            }
        } else if (selection == Selection::noreplace &&
                   std::ranges::any_of(arg.matches, [this](std::uint32_t id) {
                       return !evaluated().packages.at(id).masked;
                   })) {
            // Only an installed version emerge can keep skips it, whether or not an ebuild
            // of it is left.
            return;
        }
        if (!best && arg.atom) {
            if (auto change = autounmask(*arg.atom)) {
                propose({.change = std::move(*change), .pulled_by = {}, .named_by = arg.named});
            }
        }
        if (!best) {
            // A root set's atom may keep what is installed, and @selected's match nothing.
            const auto& set = arg.named.set;
            const bool lenient = arg.matches.empty() ? set == "selected" : !set.empty();
            if (selection == Selection::reinstall && !lenient) {
                unmatched_.insert(i);
            }
            return;
        }
        const auto& candidate = evaluated().candidates.at(*best);
        const auto id = installed_in({std::string(evaluated().string(candidate.cp)),
                                      std::string(evaluated().string(candidate.slot))});
        if (!id) {
            root_pulls_.emplace_back(i, *best);
            return;
        }
        arguments_.at(*id) = true;
        const bool matched = std::ranges::contains(arg.matches, *id);
        // Keeping the installed version would not do what is asked.
        if (selection != Selection::update || !matched) {
            must_.at(*id) = true;
            named_.at(*id) = named_.at(*id) || arg.named.set.empty();
        }
        if (selection != Selection::update) {
            forced_.insert_or_assign(*id, Forced{.candidate = best, .argument = i});
            return;
        }
        // An installed version the atom matches stays when it is the best of them.
        const auto& pkg = store().packages.at(*id);
        const auto cp = store().string(pkg.cp);
        const auto installed = version_of(store().string(pkg.cpv), cp);
        const auto to = version_of(evaluated().string(candidate.cpv), cp);
        const bool keep = matched && installed && to && vercmp(*installed, *to) >= 0;
        forced_.insert_or_assign(*id,
                                 Forced{.candidate = keep ? std::nullopt : best, .argument = i});
    }

    // Whether emerge's greedy slots leave the installed package's slot out for best, the atom's
    // best version in another slot: the best version in its slot, older than best, and best
    // block each other.
    [[nodiscard]] bool greedy_blocked(std::uint32_t id, std::uint32_t best) {
        const auto& pkg = store().packages.at(id);
        const auto& top = evaluated().candidates.at(best);
        const auto cp = store().string(pkg.cp);
        if (evaluated().string(top.slot) == store().string(pkg.slot)) {
            return false;
        }
        const auto& slot_atom = atom(std::format("{}:{}", cp, store().string(pkg.slot)));
        const auto greedy = slot_atom ? best_match(*slot_atom) : std::nullopt;
        if (!greedy) {
            return false;
        }
        const auto from =
            version_of(evaluated().string(evaluated().candidates.at(*greedy).cpv), cp);
        const auto to = version_of(evaluated().string(top.cpv), cp);
        if (!from || !to || vercmp(*from, *to) >= 0) {
            return false;
        }
        return blocks(best, *greedy) || blocks(*greedy, best);
    }

    // Whether a blocker among the candidate's dependencies matches the other candidate.
    bool blocks(std::uint32_t candidate, std::uint32_t other) {
        const Member member{.candidate = true, .index = candidate};
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            for (const auto& node : nodes(member, kind)) {
                if (node.type != NodeType::weak_blocker && node.type != NodeType::strong_blocker) {
                    continue;
                }
                const auto text = evaluated().string(node.atom);
                const auto& blocked = atom(text.substr(text.find_first_not_of('!')));
                if (blocked &&
                    matches(store(), evaluated(), evaluated().candidates.at(other), *blocked)) {
                    return true;
                }
            }
        }
        return false;
    }

    // The update of an installed package an argument decides, from its pending one.
    [[nodiscard]] std::optional<PendingUpdate>
    forced_update(std::uint32_t id, const Forced& forced,
                  std::optional<PendingUpdate> pending) const {
        const auto& atom = args_.at(forced.argument).atom;
        const auto selection = targets_ref_.get().selection;
        // -u keeps the pending update whenever the atom accepts it, a --newuse rebuild among
        // them.
        if (selection == Selection::update && atom &&
            (!pending ||
             matches(store(), evaluated(), evaluated().candidates.at(pending->target), *atom))) {
            return forced.candidate && !pending ? std::optional{update_to(id, *forced.candidate)}
                                                : pending;
        }
        if (!forced.candidate) {
            return std::nullopt;
        }
        if (pending && pending->target == *forced.candidate) {
            return pending;
        }
        return update_to(id, *forced.candidate);
    }

    // The visible versions in the candidate's slot besides it that the atom matches, best first.
    [[nodiscard]] std::vector<std::uint32_t> other_matches(std::uint32_t candidate,
                                                           const Atom& wanted) const {
        const auto& target = evaluated().candidates.at(candidate);
        struct Found {
            std::uint32_t index = 0;
            Version version;
        };
        std::vector<Found> found;
        for (const auto index : by_cp_.at(wanted.cp)) {
            const auto& other = evaluated().candidates.at(index);
            auto version = version_of(evaluated().string(other.cpv), wanted.cp);
            if (index != candidate && other.visible() && other.slot == target.slot && version &&
                matches(store(), evaluated(), other, wanted)) {
                found.push_back({.index = index, .version = std::move(*version)});
            }
        }
        std::ranges::stable_sort(
            found, [](const Found& a, const Found& b) { return vercmp(a.version, b.version) > 0; });
        std::vector<std::uint32_t> indices;
        indices.reserve(found.size());
        for (const auto& each : found) {
            indices.push_back(each.index);
        }
        return indices;
    }

    [[nodiscard]] PendingUpdate update_to(std::uint32_t id, std::uint32_t candidate) const {
        const auto& from = store().packages.at(id);
        return PendingUpdate{
            .kind = update_kind(store().string(from.cp), store().string(from.cpv),
                                evaluated().string(evaluated().candidates.at(candidate).cpv)),
            .target = candidate,
            .flags = {}};
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
        // Outside --deep's reach, emerge leaves a binding only a build needs as it was built.
        const bool built = !deep() || !reached(id);
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            if (built && (dep_kinds.at(kind) == "DEPEND" || dep_kinds.at(kind) == "BDEPEND")) {
                continue;
            }
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

    // A slot-operator binding to a sub-slot, where a package kept installed is in the slot.
    [[nodiscard]] bool stale_in_slot(std::string_view text) {
        const auto& wanted = atom(text);
        return binding(text) && wanted && kept_match(*wanted);
    }

    // Whether a package kept installed matches the atom.
    [[nodiscard]] bool kept_match(const Atom& wanted) const {
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            if (kept(id) && matches(store(), store().packages.at(id), wanted)) {
                return true;
            }
        }
        return false;
    }

    // The first slot-operator binding of the installed package to a sub-slot nothing installed
    // is in any more, where something installed is in the slot: what emerge -uD's unsatisfied
    // slot-operator probe rebuilds the package for, with that package (or what replaces it) as
    // the reason.
    std::optional<Reason> stale_binding(std::uint32_t id) {
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            for (const auto& node : nodes({.candidate = false, .index = id}, kind)) {
                if (node.type != NodeType::atom || node.matches.count != 0) {
                    continue;
                }
                const auto text = store().string(node.atom);
                const auto& parsed = atom(text);
                if (!binding(text) || !parsed) {
                    continue;
                }
                for (std::uint32_t other = 0; other < store().packages.size(); ++other) {
                    if (!matches(store(), store().packages.at(other), *parsed)) {
                        continue;
                    }
                    const auto merged = choices_.at(other).merged();
                    return Reason{.member = merged ? Member{.candidate = true, .index = *merged}
                                                   : Member{.candidate = false, .index = other},
                                  .atom = std::string(text)};
                }
            }
        }
        return std::nullopt;
    }

    // Whether -uD rebuilds the installed package for its broken bindings: within the reach, or
    // once each merge breaking one breaks a binding within the reach too. emerge's slot-operator
    // backtracking starts only from what the arguments reach, and otherwise drops the merge.
    [[nodiscard]] bool triggered(std::uint32_t id, std::span<const Broken> broken) const {
        return reached(id) || std::ranges::all_of(broken, [this](const Broken& each) {
                   return triggers_.contains(each.replaced);
               });
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
             .by = Reason{.member = {.candidate = false, .index = id}, .atom = moved.wanted},
             .named_by = {},
             .root = {}});
        work.push_back({.member = {.candidate = true, .index = moved.candidate}, .root = {}});
        return Member{.candidate = true, .index = moved.candidate};
    }

    // What a slot-operator rebuild merges: outside --deep's reach, emerge's update probe takes
    // the best visible version in the slot.
    [[nodiscard]] std::optional<std::uint32_t> rebuild_target(std::uint32_t id) const {
        if (const auto target = evaluated().packages.at(id).target;
            target && (!deep() || !reached(id))) {
            return target;
        }
        return rebuild_of(id);
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

    [[nodiscard]] std::optional<std::uint32_t> best_match(const Atom& wanted) const {
        const auto found = by_cp_.find(wanted.cp);
        if (found == by_cp_.end()) {
            return std::nullopt;
        }
        std::optional<std::uint32_t> best;
        std::optional<Version> best_version;
        for (const auto index : found->second) {
            const auto& candidate = evaluated().candidates.at(index);
            if (!candidate.visible() || backtracked_.contains(index) ||
                !matches(store(), evaluated(), candidate, wanted)) {
                continue;
            }
            auto version = version_of(evaluated().string(candidate.cpv), wanted.cp);
            if (version && (!best_version || vercmp(*version, *best_version) > 0)) {
                best = index;
                best_version = std::move(version);
            }
        }
        return best;
    }

    // The best visible candidate that matches the atom, if its slot is free.
    std::optional<std::uint32_t> pullable(const Atom& wanted) {
        const auto best = best_match(wanted);
        if (!best) {
            return std::nullopt;
        }
        const auto& candidate = evaluated().candidates.at(*best);
        const SlotKey key{std::string(evaluated().string(candidate.cp)),
                          std::string(evaluated().string(candidate.slot))};
        if (taken_.contains(key)) {
            return std::nullopt;
        }
        if (const auto id = installed_in(key)) {
            const auto cp = evaluated().string(candidate.cp);
            const auto from = version_of(store().string(store().packages.at(*id).cpv), cp);
            const auto to = version_of(evaluated().string(candidate.cpv), cp);
            // A newer version, the same one built with changed USE, or whatever matches when
            // the installed package fails only the atom's USE dependencies, built without them.
            const auto& pkg = store().packages.at(*id);
            auto plain = wanted;
            plain.use.clear();
            const bool unmet = !matches(store(), pkg, wanted) && matches(store(), pkg, plain);
            if (!choices_.at(*id).wanted && from && to &&
                (unmet || vercmp(*to, *from) > 0 ||
                 (vercmp(*to, *from) == 0 && changed_.contains(*best)))) {
                wanted_.emplace_back(*id, *best);
            }
            return std::nullopt;
        }
        return best;
    }

    // What -uD moves node index of list to when what the plan holds satisfies it, appended to
    // moves: for each atom naming no slot (within a ||, its first alternative satisfied), the
    // best visible version when it is new in its slot, unless emerge's graph holds a match at
    // least as high (an argument's installed package) or an installed match with a visible
    // ebuild is higher; emerge passes over one without once an ebuild matched.
    void slot_moves(const Tables& tables, std::span<const Node> list, std::size_t index,
                    const std::vector<bool>& ok,
                    std::vector<std::pair<std::uint32_t, std::string>>& moves) {
        const auto& node = element(list, index);
        if (node.type == NodeType::any_of || node.type == NodeType::all_of) {
            for (std::size_t child = index + 1; child < list.size(); ++child) {
                if (element(list, child).parent != index) {
                    continue;
                }
                if (node.type == NodeType::all_of) {
                    slot_moves(tables, list, child, ok, moves);
                } else if (ok.at(child)) {
                    slot_moves(tables, list, child, ok, moves);
                    return;
                }
            }
            return;
        }
        if (node.type != NodeType::atom) {
            return;
        }
        const auto text = tables.string(node.atom);
        const auto& wanted = atom(text);
        if (!wanted || wanted->slot) {
            return;
        }
        const auto best = best_match(*wanted);
        if (!best) {
            return;
        }
        const auto& candidate = evaluated().candidates.at(*best);
        const auto cp = evaluated().string(candidate.cp);
        const SlotKey key{std::string(cp), std::string(evaluated().string(candidate.slot))};
        const auto to = version_of(evaluated().string(candidate.cpv), cp);
        if (!to || taken_.contains(key) || installed_in(key)) {
            return;
        }
        for (const auto id : tables.ids_in(node.matches)) {
            const auto from = version_of(store().string(store().packages.at(id).cpv), cp);
            if (!from) {
                return;
            }
            const auto order = vercmp(*from, *to);
            if ((argument(id) && order >= 0) ||
                (evaluated().packages.at(id).visible && order > 0)) {
                return;
            }
        }
        moves.emplace_back(*best, std::string(text));
    }

    [[nodiscard]] std::optional<std::uint32_t> installed_in(const SlotKey& key) const {
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            const auto& pkg = store().packages.at(id);
            if (store().string(pkg.cp) == key.first && store().string(pkg.slot) == key.second) {
                return id;
            }
        }
        return std::nullopt;
    }

    // An installed package with no update of its own (without --deep, or out of scope) is
    // replaced when a merge needs a newer version: the candidate becomes its one option.
    void need(std::uint32_t id, std::uint32_t candidate) {
        auto& choice = choices_.at(id);
        if (choice.wanted) {
            return;
        }
        const auto& from = store().packages.at(id);
        choice.options = {candidate};
        // A rebuild for changed USE shows the changed flags as --newuse does.
        std::string flags;
        if (const auto changed = changed_.find(candidate); changed != changed_.end()) {
            for (const auto& [flag, on] : changed->second) {
                flags += std::format("{}{}{}*", flags.empty() ? "" : " ", on ? "" : "-", flag);
            }
        } else if (const auto& pkg = evaluated().packages.at(id); pkg.target == candidate) {
            // Rebuilt for its configured USE, which it was built without.
            for (const auto flag : evaluated().ids_in(pkg.rebuild)) {
                flags += std::format("{}{}", flags.empty() ? "" : " ", evaluated().string(flag));
            }
        }
        choice.wanted = PendingUpdate{
            .kind = update_kind(store().string(from.cp), store().string(from.cpv),
                                evaluated().string(evaluated().candidates.at(candidate).cpv)),
            .target = candidate,
            .flags = std::move(flags)};
        choice.induced = true;
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
                // emerge's autounmask only runs when nothing matches, an installed package
                // included: one a merge replaces holds that merge back instead.
                if (wanted && puller_ && tables.ids_in(node.matches).empty() &&
                    !best_match(*wanted)) {
                    if (auto change = autounmask(*wanted)) {
                        pending_.push_back(
                            {.change = std::move(*change),
                             .pulled_by = Reason{.member = *puller_, .atom = std::string(text)},
                             .named_by = {}});
                    }
                }
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
            // A || asks for a USE change only when no alternative will do without, and then
            // of the first that can take one.
            const auto asked = pending_.size();
            bool some = false;
            for (std::size_t child = index + 1; child < list.size(); ++child) {
                if (element(list, child).parent != index) {
                    continue;
                }
                some = true;
                const auto before = pulls.size();
                const bool done = pull(tables, list, child, pulls);
                if (any && done) {
                    pending_.resize(asked);
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
            if (any && pending_.size() > asked + 1) {
                pending_.resize(asked + 1);
            }
            return !any || !some;
        }
        }
        return false;
    }

    // What emerge's autounmask changes to meet wanted's USE dependencies: the best visible
    // version that can meet them, with the flags; none when no version can.
    [[nodiscard]] std::optional<UseChange> autounmask(const Atom& wanted) const {
        const auto found = by_cp_.find(wanted.cp);
        if (wanted.use.empty() || found == by_cp_.end()) {
            return std::nullopt;
        }
        auto plain = wanted;
        plain.use.clear();
        std::vector<std::pair<Version, std::uint32_t>> options;
        for (const auto index : found->second) {
            const auto& candidate = evaluated().candidates.at(index);
            if (!candidate.visible() || backtracked_.contains(index) ||
                !matches(store(), evaluated(), candidate, plain)) {
                continue;
            }
            if (auto version = version_of(evaluated().string(candidate.cpv), wanted.cp)) {
                options.emplace_back(std::move(*version), index);
            }
        }
        std::ranges::stable_sort(
            options, [](const auto& a, const auto& b) { return vercmp(a.first, b.first) > 0; });
        for (const auto& [version, index] : options) {
            if (auto change = use_change(index, wanted)) {
                return change;
            }
        }
        return std::nullopt;
    }

    // The flags of the candidate to change, on top of those changed already, for wanted's USE
    // dependencies to hold; none when they cannot be or need not be.
    [[nodiscard]] std::optional<UseChange> use_change(std::uint32_t index,
                                                      const Atom& wanted) const {
        const auto& candidate = evaluated().candidates.at(index);
        const auto in = [&](Range range, std::string_view flag) {
            return std::ranges::any_of(evaluated().ids_in(range), [&](std::uint32_t id) {
                return evaluated().string(id) == flag;
            });
        };
        UseChange change{.candidate = index, .flags = {}};
        if (const auto earlier = changed_.find(index); earlier != changed_.end()) {
            change.flags = earlier->second;
        }
        bool needed = false;
        for (const auto& dep : wanted.use) {
            if (!has_flag(store(), evaluated(), candidate, dep.flag)) {
                // Its default decides, and the user cannot change that.
                const bool counts = dep.fallback == UseDependency::Default::enabled;
                if (dep.fallback == UseDependency::Default::none || counts != dep.enabled) {
                    return std::nullopt;
                }
                continue;
            }
            if (in(candidate.use, dep.flag) == dep.enabled) {
                continue;
            }
            if (in(candidate.forced, dep.flag)) {
                return std::nullopt;
            }
            const auto [at, added] = change.flags.try_emplace(dep.flag, dep.enabled);
            if (!added && at->second != dep.enabled) {
                return std::nullopt;
            }
            needed = true;
        }
        return needed ? std::optional{std::move(change)} : std::nullopt;
    }

    // Keeps a USE change, the first asked of its candidate.
    void propose(NeededUseChange needed) {
        if (std::ranges::none_of(proposed_, [&](const NeededUseChange& each) {
                return each.change.candidate == needed.change.candidate;
            })) {
            proposed_.push_back(std::move(needed));
        }
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
                if (!kept(id) && (!argument(id) || alone(id))) {
                    return std::pair{id, std::string(tables.string(node.atom))};
                }
            }
        }
        return std::nullopt;
    }

    // Selects each of unselected that an atom of the member's dependencies matches: every kind
    // of a candidate's, only the run-time ones of an installed package, whose build-time ones
    // emerge passes over.
    void
    select_matched(const Tables& tables, const Member& member,
                   std::map<std::string, std::vector<std::uint32_t>, std::less<>>& unselected) {
        for (std::size_t kind = 0; kind < dep_kinds.size() && !unselected.empty(); ++kind) {
            if (!member.candidate &&
                (dep_kinds.at(kind) == "DEPEND" || dep_kinds.at(kind) == "BDEPEND")) {
                continue;
            }
            for (const auto& node : nodes(member, kind)) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                const auto& wanted = atom(tables.string(node.atom));
                if (!wanted) {
                    continue;
                }
                const auto found = unselected.find(wanted->cp);
                if (found == unselected.end()) {
                    continue;
                }
                std::erase_if(found->second, [&](std::uint32_t candidate) {
                    if (!matches(store(), evaluated(), evaluated().candidates.at(candidate),
                                 *wanted)) {
                        return false;
                    }
                    selected_.insert(candidate);
                    return true;
                });
                if (found->second.empty()) {
                    unselected.erase(found);
                }
            }
        }
    }

    // One pass over the current choices: false once none of them moved.
    bool settle() {
        pulled_.clear();
        present_.clear();
        taken_.clear();
        rebuilt_.clear();
        needed_.clear();
        missing_.clear();
        std::vector<Work> work;
        // Merged updates emerge selects only through an atom that matches them: an argument's,
        // or a dependency's of what it traverses. By cp.
        std::map<std::string, std::vector<std::uint32_t>, std::less<>> unselected;
        for (std::uint32_t id = 0; id < choices_.size(); ++id) {
            if (const auto merged = choices_.at(id).merged()) {
                place(*merged);
                if (argument(id)) {
                    selected_.insert(*merged);
                } else {
                    unselected[std::string(
                                   evaluated().string(evaluated().candidates.at(*merged).cp))]
                        .push_back(*merged);
                }
                work.push_back({.member = {.candidate = true, .index = *merged}, .root = id});
            }
        }
        triggers_.clear();
        if (deep() && !targets_ref_.get().reach.empty()) {
            for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
                if (kept(id) && in_scope(id) && reached(id)) {
                    for (const auto& each : broken_bindings(id)) {
                        triggers_.insert(each.replaced);
                    }
                }
            }
        }
        for (auto& [root, candidate] : root_pulls_) {
            if (backtracked_.contains(candidate) && !fall_back(root, candidate)) {
                continue;
            }
            const auto& c = evaluated().candidates.at(candidate);
            if (taken_.contains({std::string(evaluated().string(c.cp)),
                                 std::string(evaluated().string(c.slot))})) {
                continue;
            }
            place(candidate);
            selected_.insert(candidate);
            pulled_.push_back({.candidate = candidate, .by = {}, .named_by = root, .root = {}});
            work.push_back({.member = {.candidate = true, .index = candidate}, .root = {}});
        }
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            if (kept(id) && in_scope(id)) {
                work.push_back({.member = {.candidate = false, .index = id}, .root = {}});
            }
        }
        std::map<std::uint32_t, std::vector<Reason>> rejected;
        std::set<std::uint32_t> unsatisfiable;
        bool masked = false;
        for (std::size_t w = 0; w < work.size(); ++w) {
            const auto item = work.at(w);
            if (item.root && rejected.contains(*item.root)) {
                continue;
            }
            if (!item.member.candidate) {
                // A rebuild's own dependencies stand in for the installed ones. Plain -u
                // rebuilds nothing installed: the binding holds the merge back.
                if (const auto broken = broken_bindings(item.member.index); !broken.empty()) {
                    // Plain emerge rebuilds them for what it is asked to merge.
                    const bool rebuilds = (deep() && triggered(item.member.index, broken) &&
                                           std::ranges::none_of(broken,
                                                                [this](const Broken& each) {
                                                                    return alone(each.replaced);
                                                                })) ||
                                          std::ranges::any_of(broken, [this](const Broken& each) {
                                              return must(each.replaced);
                                          });
                    if (const auto own =
                            rebuilds ? rebuild_target(item.member.index) : std::nullopt) {
                        const auto& first = broken.front();
                        rebuilt_.emplace(
                            item.member.index,
                            Rebuilt{.candidate = *own,
                                    .why = {.member = {.candidate = true, .index = first.merged},
                                            .atom = first.atom}});
                        selected_.insert(*own);
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
                if (deep() && reached(item.member.index)) {
                    if (const auto own = rebuild_of(item.member.index)) {
                        if (auto why = stale_binding(item.member.index)) {
                            rebuilt_.emplace(item.member.index,
                                             Rebuilt{.candidate = *own, .why = std::move(*why)});
                            selected_.insert(*own);
                            work.push_back(
                                {.member = {.candidate = true, .index = *own}, .root = {}});
                            continue;
                        }
                    }
                }
                if (const auto moved = new_slots_.find(item.member.index);
                    moved != new_slots_.end()) {
                    if (const auto child = moved_to(item.member.index, moved->second, work)) {
                        const auto& found = moved->second;
                        rebuilt_.emplace(item.member.index,
                                         Rebuilt{.candidate = found.own,
                                                 .why = {.member = *child, .atom = found.bound}});
                        selected_.insert(found.own);
                        work.push_back({.member = {.candidate = true, .index = found.own},
                                        .root = child->candidate ? found.installed : std::nullopt});
                        continue;
                    }
                }
            }
            const auto& tables = this->tables(item.member);
            if (item.member.candidate || (deep() && reached(item.member.index))) {
                select_matched(tables, item.member, unselected);
            }
            // What emerge would reach of what this member pulls: (rank, in a ||, candidate), and
            // the first rank with an atom nothing satisfies.
            std::vector<std::tuple<std::size_t, bool, std::uint32_t>> reached_pulls;
            std::optional<std::size_t> stopped;
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                const auto rank = emerge_rank(kind);
                const auto list = nodes(item.member, kind);
                const auto ok = satisfied_nodes(tables, list);
                // An installed package's build-time dependencies emerge passes over.
                const bool moves =
                    deep() && (item.member.candidate ||
                               (reached(item.member.index) && dep_kinds.at(kind) != "DEPEND" &&
                                dep_kinds.at(kind) != "BDEPEND"));
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (element(list, i).parent != no_parent) {
                        continue;
                    }
                    if (ok.at(i)) {
                        if (!moves) {
                            continue;
                        }
                        std::vector<std::pair<std::uint32_t, std::string>> found;
                        slot_moves(tables, list, i, ok, found);
                        for (auto& [candidate, text] : found) {
                            place(candidate);
                            reached_pulls.emplace_back(rank, false, candidate);
                            pulled_.push_back(
                                {.candidate = candidate,
                                 .by = Reason{.member = item.member, .atom = std::move(text)},
                                 .named_by = {},
                                 .root = item.root});
                            work.push_back({.member = {.candidate = true, .index = candidate},
                                            .root = item.root});
                        }
                        continue;
                    }
                    // Plain -u only checks that a kept package's dependencies stay satisfied:
                    // what a merge takes away is rejected, and what was missing stays so.
                    if (!item.member.candidate && (!deep() || !reached(item.member.index))) {
                        if (const auto held = replaced_match(tables, list, i)) {
                            rejected[held->first].push_back(
                                {.member = item.member, .atom = held->second});
                        }
                        continue;
                    }
                    if (const auto held = installed_alternative(tables, list, i)) {
                        rejected[held->first].push_back(
                            {.member = item.member, .atom = held->second});
                        continue;
                    }
                    std::vector<std::pair<std::uint32_t, std::string>> pulls;
                    wanted_.clear();
                    const bool disjunctive = element(list, i).type == NodeType::any_of;
                    puller_ = item.member;
                    pending_.clear();
                    const bool pulled_in = pull(tables, list, i, pulls);
                    puller_.reset();
                    if (!pulled_in) {
                        for (auto& needed : pending_) {
                            propose(std::move(needed));
                        }
                    }
                    if (pulled_in) {
                        for (auto& [candidate, text] : pulls) {
                            reached_pulls.emplace_back(rank, disjunctive, candidate);
                            pulled_.push_back(
                                {.candidate = candidate,
                                 .by = Reason{.member = item.member, .atom = std::move(text)},
                                 .named_by = {},
                                 .root = item.root});
                            work.push_back({.member = {.candidate = true, .index = candidate},
                                            .root = item.root});
                        }
                        continue;
                    }
                    // The first installed package in the way takes the update, and the next
                    // pass weighs it.
                    if (!wanted_.empty()) {
                        needed_.insert(wanted_.front());
                        continue;
                    }
                    const Reason failed{.member = item.member, .atom = first_atom(tables, list, i)};
                    if (const auto held = replaced_match(tables, list, i)) {
                        rejected[held->first].push_back(
                            {.member = item.member, .atom = held->second});
                        continue;
                    }
                    if (!item.member.candidate && dep_kinds.at(kind) != "DEPEND" &&
                        dep_kinds.at(kind) != "BDEPEND" &&
                        (!lenient_ ||
                         (matched_without_use(failed.atom) && !stale_in_slot(failed.atom)))) {
                        // -uD must satisfy what it reaches and keeps, but for what nothing
                        // matches at all, which it only skips for the world sets' packages.
                        missing_.push_back(failed);
                    } else if (pulled(item.member)) {
                        // emerge masks what it pulled in and chooses again.
                        masked =
                            backtracked_.try_emplace(item.member.index, failed).second || masked;
                    } else if (item.root) {
                        const auto found = leaves(failed);
                        // Not where a version that would do is kept out, by a hold on its slot.
                        if (std::ranges::any_of(found, [this](const Reason& leaf) {
                                return !matched_without_use(leaf.atom);
                            })) {
                            unsatisfiable.insert(*item.root);
                        }
                        std::ranges::copy(found, std::back_inserter(rejected[*item.root]));
                    } else {
                        continue;
                    }
                    if (!disjunctive) {
                        stopped = std::min(stopped.value_or(rank), rank);
                    }
                }
            }
            for (const auto& [rank, disjunctive, candidate] : reached_pulls) {
                if (!stopped || (!disjunctive && rank < *stopped)) {
                    selected_.insert(candidate);
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
                choice.unsatisfiable = unsatisfiable.contains(id);
            }
            ++choice.at;
        }
        for (const auto& [id, candidate] : needed_) {
            need(id, candidate);
        }
        return !rejected.empty() || !needed_.empty() || masked;
    }

    // Moves argument root's pull on from its backtracked candidate to the next best version, new
    // in its slot; false when there is none, the argument refused, or when the next is in an
    // installed slot, which takes it as select() has it: plain emerge reinstalls, -u only moves
    // up.
    bool fall_back(std::uint32_t root, std::uint32_t& candidate) {
        const auto& atom = args_.at(root).atom;
        const auto next = atom ? best_match(*atom) : std::nullopt;
        if (!next) {
            refused_.insert(root);
            return false;
        }
        const auto& c = evaluated().candidates.at(*next);
        const auto cp = evaluated().string(c.cp);
        const auto id = installed_in({std::string(cp), std::string(evaluated().string(c.slot))});
        if (!id) {
            candidate = *next;
            return true;
        }
        if (!choices_.at(*id).wanted) {
            const auto from = version_of(store().string(store().packages.at(*id).cpv), cp);
            const auto to = version_of(evaluated().string(c.cpv), cp);
            if (targets_ref_.get().selection != Selection::update ||
                (from && to && vercmp(*to, *from) > 0)) {
                needed_.emplace(*id, *next);
            }
        }
        return false;
    }

    // Whether an installed package or a visible candidate matches the atom without its USE
    // dependencies.
    [[nodiscard]] bool matched_without_use(std::string_view text) const {
        auto parsed = parse_atom(text);
        if (!parsed) {
            return false;
        }
        parsed->use.clear();
        if (std::ranges::any_of(store().packages, [&](const Package& pkg) {
                return matches(store(), pkg, *parsed);
            })) {
            return true;
        }
        const auto found = by_cp_.find(parsed->cp);
        return found != by_cp_.end() && std::ranges::any_of(found->second, [&](std::uint32_t i) {
                   const auto& candidate = evaluated().candidates.at(i);
                   return candidate.visible() && matches(store(), evaluated(), candidate, *parsed);
               });
    }

    // Whether the member is a candidate this pass pulled in, rather than one a choice merges.
    [[nodiscard]] bool pulled(const Member& member) const {
        return member.candidate && std::ranges::any_of(pulled_, [&](const Pulled& each) {
                   return each.candidate == member.index;
               });
    }

    // The reason's dependency, followed through the candidates backtracking masked for it to
    // the dependencies no visible version matches.
    [[nodiscard]] std::vector<Reason> leaves(const Reason& reason) {
        std::vector<Reason> found;
        std::set<std::uint32_t> seen;
        collect_leaves(reason, seen, found);
        std::ranges::sort(found);
        const auto [first, last] = std::ranges::unique(found);
        found.erase(first, last);
        return found;
    }

    void add_unsatisfied(Plan& plan, const Reason& reason) {
        for (auto& leaf : leaves(reason)) {
            plan.unsatisfied.push_back({.member = leaf.member, .atom = std::move(leaf.atom)});
        }
    }

    void collect_leaves(const Reason& reason, std::set<std::uint32_t>& seen,
                        std::vector<Reason>& found) {
        const auto& wanted = atom(reason.atom);
        bool followed = false;
        for (const auto& [candidate, why] : backtracked_) {
            if (wanted &&
                matches(store(), evaluated(), evaluated().candidates.at(candidate), *wanted)) {
                followed = true;
                if (seen.insert(candidate).second) {
                    collect_leaves(why, seen, found);
                }
            }
        }
        if (!followed) {
            found.push_back(reason);
        }
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

    [[nodiscard]] Plan result() {
        Plan plan;
        for (std::uint32_t id = 0; id < choices_.size(); ++id) {
            if (const auto found = rebuilt_.find(id); found != rebuilt_.end()) {
                const auto& candidate = evaluated().candidates.at(found->second.candidate);
                plan.merges.push_back(
                    {.candidate = found->second.candidate,
                     .replaces = id,
                     .kind = update_kind(evaluated().string(candidate.cp),
                                         store().string(store().packages.at(id).cpv),
                                         evaluated().string(candidate.cpv)),
                     .flags = {},
                     .pulled_by = {},
                     .named_by = {},
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
                            .named_by = {},
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
            if (!choice.merged() && named(id)) {
                // What an argument must merge, with no version left to merge.
                for (const auto& reason : choice.reasons) {
                    add_unsatisfied(plan, reason);
                }
            } else if (choice.at != 0 && !choice.induced) {
                plan.held.push_back({.package = id,
                                     .wanted = *choice.wanted,
                                     .reasons = choice.reasons,
                                     .unsatisfiable = choice.unsatisfiable});
            }
        }
        auto pulled = pulled_;
        std::ranges::sort(pulled, [this](const Pulled& a, const Pulled& b) {
            return evaluated().string(evaluated().candidates.at(a.candidate).cpv) <
                   evaluated().string(evaluated().candidates.at(b.candidate).cpv);
        });
        for (const auto& reason : missing_) {
            add_unsatisfied(plan, reason);
        }
        for (const auto root : unmatched_) {
            plan.unsatisfied.push_back({.member = {}, .atom = args_.at(root).named.atom});
        }
        for (const auto root : refused_) {
            for (const auto& [candidate, why] : backtracked_) {
                if (const auto& wanted = args_.at(root).atom;
                    wanted &&
                    matches(store(), evaluated(), evaluated().candidates.at(candidate), *wanted)) {
                    add_unsatisfied(plan, why);
                }
            }
        }
        std::ranges::sort(plan.unsatisfied);
        const auto [first, last] = std::ranges::unique(plan.unsatisfied);
        plan.unsatisfied.erase(first, last);
        for (const auto candidate : selected_) {
            if (!required_use_of(evaluated(), evaluated().candidates.at(candidate)).satisfied) {
                plan.unmet.push_back(candidate);
            }
        }
        for (const auto& found : pulled) {
            plan.merges.push_back({.candidate = found.candidate,
                                   .replaces = {},
                                   .kind = UpdateKind::upgrade,
                                   .flags = {},
                                   .pulled_by = found.by,
                                   .named_by = found.named_by
                                                   ? std::optional{args_.at(*found.named_by).named}
                                                   : std::nullopt,
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
    // The merges other than self an atom matches.
    const auto matching = [&](std::string_view text, std::optional<std::uint32_t> self) {
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
        std::vector<std::uint32_t> found;
        const auto& atom = parsed->second;
        if (!atom) {
            return found;
        }
        if (const auto same = by_cp.find(atom->cp); same != by_cp.end()) {
            for (const auto other : same->second) {
                if (other != self && matches(store, evaluated, candidate(other), *atom)) {
                    found.push_back(other);
                }
            }
        }
        return found;
    };
    // The merges each one waits for, with the kinds it does by.
    std::vector<std::map<std::uint32_t, WaitKinds>> needs(count);
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        for (std::uint32_t kind = 0; kind < dep_kinds.size(); ++kind) {
            const auto name = dep_kinds.at(kind);
            for (const auto& node : evaluated.nodes_in(candidate(merge).deps.at(kind))) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                for (const auto other : matching(evaluated.string(node.atom), merge)) {
                    auto& kinds = needs.at(merge)[other];
                    kinds.build = kinds.build || is_build_kind(kind);
                    kinds.install = kinds.install || name == "IDEPEND";
                    kinds.run = kinds.run || name == "RDEPEND";
                    kinds.post = kinds.post || name == "PDEPEND";
                }
            }
        }
    }
    // What each installed package leads to: those it depends on that stay, and the merges its
    // atoms match, the replacement of one it depends on among them.
    std::map<std::uint32_t, std::uint32_t> replacing;
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        if (const auto id = plan.merges.at(merge).replaces) {
            replacing.emplace(*id, merge);
        }
    }
    const auto installed_count = store.packages.size();
    std::vector<std::vector<std::uint32_t>> stays(installed_count);
    std::vector<std::vector<std::uint32_t>> leads(installed_count);
    for (std::uint32_t id = 0; id < installed_count; ++id) {
        for (const auto range : store.packages.at(id).deps) {
            for (const auto& node : store.nodes_in(range)) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                for (const auto match : store.ids_in(node.matches)) {
                    if (const auto found = replacing.find(match); found != replacing.end()) {
                        leads.at(id).push_back(found->second);
                    } else {
                        stays.at(id).push_back(match);
                    }
                }
                std::ranges::copy(matching(store.string(node.atom), std::nullopt),
                                  std::back_inserter(leads.at(id)));
            }
        }
    }
    // A merge reaches through the installed packages that stay what they lead to, as emerge's
    // scheduler waits for any merge its graph reaches.
    std::vector<std::uint32_t> stack;
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        std::vector<bool> seen(installed_count);
        for (const auto range : candidate(merge).deps) {
            for (const auto& node : evaluated.nodes_in(range)) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                for (const auto id : evaluated.ids_in(node.matches)) {
                    if (!replacing.contains(id)) {
                        stack.push_back(id);
                    }
                }
            }
        }
        while (!stack.empty()) {
            const auto id = stack.back();
            stack.pop_back();
            if (seen.at(id)) {
                continue;
            }
            seen.at(id) = true;
            for (const auto other : leads.at(id)) {
                if (other != merge) {
                    needs.at(merge)[other].through = true;
                }
            }
            std::ranges::copy(stays.at(id), std::back_inserter(stack));
        }
    }
    // Only a run-time dependency holds it back.
    const auto run_only = [](const WaitKinds& kinds) { return !kinds.build && !kinds.install; };
    // Only a PDEPEND, which emerge's cycles drop first.
    const auto post_only = [](const WaitKinds& kinds) {
        return kinds.post && !kinds.build && !kinds.install && !kinds.run;
    };
    // emerge has every merge wait for libc (virtual/libc's provider): it goes first, with what
    // it waits for.
    std::vector<bool> libc(count);
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
                        for (const auto merge : same->second) {
                            libc.at(merge) = true;
                        }
                    }
                }
            }
        }
    }
    std::vector<bool> early(count);
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        if (libc.at(merge)) {
            stack.push_back(merge);
        }
    }
    while (!stack.empty()) {
        const auto merge = stack.back();
        stack.pop_back();
        if (!early.at(merge)) {
            early.at(merge) = true;
            for (const auto& [other, kinds] : needs.at(merge)) {
                if (ordering(kinds)) {
                    stack.push_back(other);
                }
            }
        }
    }
    std::vector<bool> placed(count);
    while (plan.order.size() < count) {
        // Ready first, then ready but for waits through installed packages, then but for
        // PDEPEND's waits, then but for run-time ones, then the fewest waits left; libc first
        // among them, then plan order.
        std::optional<std::tuple<int, bool, std::size_t, std::size_t, std::uint32_t>> best;
        for (std::uint32_t merge = 0; merge < count; ++merge) {
            if (placed.at(merge)) {
                continue;
            }
            std::size_t build = 0;
            std::size_t all = 0;
            std::size_t post = 0;
            bool through = false;
            for (const auto& [other, kinds] : needs.at(merge)) {
                if (placed.at(other)) {
                    continue;
                }
                if (ordering(kinds)) {
                    ++all;
                    build += run_only(kinds) ? 0U : 1U;
                    post += post_only(kinds) ? 1U : 0U;
                }
                through = through || kinds.through;
            }
            const std::tuple key{all == 0 && !through ? 0
                                 : all == 0           ? 1
                                 : all == post        ? 2
                                 : build == 0         ? 3
                                                      : 4,
                                 !early.at(merge), build, all, merge};
            if (!best || key < *best) {
                best = key;
            }
        }
        if (!best) {
            break;
        }
        const auto merge = std::get<4>(*best);
        placed.at(merge) = true;
        plan.order.push_back(merge);
    }
    // Every later merge waits for a libc merged before it, but for a rebuild of the installed
    // version and the other libcs.
    std::set<std::string_view> installed;
    for (const auto& pkg : store.packages) {
        installed.insert(store.string(pkg.cpv));
    }
    std::vector<std::uint32_t> libcs;
    for (const auto merge : plan.order) {
        if (!libc.at(merge)) {
            for (const auto other : libcs) {
                needs.at(merge)[other].libc = true;
            }
        } else if (!installed.contains(evaluated.string(candidate(merge).cpv))) {
            libcs.push_back(merge);
        }
    }
    for (std::uint32_t merge = 0; merge < count; ++merge) {
        for (const auto& [other, kinds] : needs.at(merge)) {
            plan.merges.at(merge).waits.push_back({.merge = other, .kinds = kinds});
        }
    }
}

} // namespace

std::string wait_letters(const WaitKinds& kinds) {
    std::string letters;
    for (const auto& [kind, letter] :
         {std::pair{kinds.build, 'b'}, std::pair{kinds.install, 'i'}, std::pair{kinds.run, 'r'},
          std::pair{kinds.post, 'p'}, std::pair{kinds.libc, 'l'}, std::pair{kinds.through, 't'}}) {
        if (kind) {
            letters += letter;
        }
    }
    return letters;
}

std::vector<std::uint32_t> other_slots(const Store& store, const Evaluated& evaluated,
                                       const Merge& merge) {
    if (merge.replaces) {
        return {};
    }
    const auto cp = evaluated.string(evaluated.candidates.at(merge.candidate).cp);
    struct Found {
        std::uint32_t id = 0;
        std::optional<Version> version;
    };
    std::vector<Found> found;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        const auto& pkg = store.packages.at(id);
        if (store.string(pkg.cp) == cp) {
            found.push_back({.id = id, .version = version_of(store.string(pkg.cpv), cp)});
        }
    }
    std::ranges::stable_sort(found, [](const Found& a, const Found& b) {
        return a.version && b.version && vercmp(*a.version, *b.version) < 0;
    });
    std::vector<std::uint32_t> ids;
    ids.reserve(found.size());
    for (const auto& each : found) {
        ids.push_back(each.id);
    }
    return ids;
}

std::string package_use_line(const Store& store, const Evaluated& evaluated,
                             const UseChange& change) {
    const auto& candidate = evaluated.candidates.at(change.candidate);
    const auto cp = evaluated.string(candidate.cp);
    const auto version = version_of(evaluated.string(candidate.cpv), cp);
    bool latest = true;
    bool latest_in_slot = true;
    const auto newer = [&](std::string_view cpv, std::string_view slot) {
        const auto other = version_of(cpv, cp);
        if (version && other && vercmp(*other, *version) > 0) {
            latest = false;
            latest_in_slot = latest_in_slot && slot != evaluated.string(candidate.slot);
        }
    };
    for (const auto& other : evaluated.candidates) {
        if (other.visible() && evaluated.string(other.cp) == cp) {
            newer(evaluated.string(other.cpv), evaluated.string(other.slot));
        }
    }
    for (const auto& pkg : store.packages) {
        if (store.string(pkg.cp) == cp) {
            newer(store.string(pkg.cpv), store.string(pkg.slot));
        }
    }
    auto line = latest           ? std::format(">={}", evaluated.string(candidate.cpv))
                : latest_in_slot ? std::format(">={}:{}", evaluated.string(candidate.cpv),
                                               evaluated.string(candidate.slot))
                                 : std::format("={}", evaluated.string(candidate.cpv));
    for (const auto& [flag, on] : change.flags) {
        line += std::format(" {}{}", on ? "" : "-", flag);
    }
    return line;
}

std::vector<std::string> required_by(const Store& store, const Evaluated& evaluated,
                                     const Plan& plan, const NeededUseChange& needed) {
    std::vector<std::string> chain;
    const auto argument = [&chain](const Argument& named) {
        chain.push_back(named.set.empty() ? std::format("{} (argument)", named.atom)
                                          : std::format("@{}", named.set));
    };
    if (needed.named_by) {
        argument(*needed.named_by);
        return chain;
    }
    std::optional<Member> at =
        needed.pulled_by ? std::optional{needed.pulled_by->member} : std::nullopt;
    std::set<std::uint32_t> seen;
    while (at) {
        if (!at->candidate) {
            const auto& pkg = store.packages.at(at->index);
            chain.push_back(std::format("{}::{}", store.string(pkg.cpv), store.string(pkg.repo)));
            break;
        }
        const auto& candidate = evaluated.candidates.at(at->index);
        chain.push_back(std::format("{}::{}", evaluated.string(candidate.cpv),
                                    evaluated.string(candidate.repo)));
        const auto merge = std::ranges::find_if(
            plan.merges, [&](const Merge& each) { return each.candidate == at->index; });
        if (!seen.insert(at->index).second || merge == plan.merges.end()) {
            break;
        }
        if (merge->pulled_by) {
            at = merge->pulled_by->member;
        } else {
            if (merge->named_by) {
                argument(*merge->named_by);
            }
            at.reset();
        }
    }
    return chain;
}

RequiredUse required_use_of(const Evaluated& evaluated, const Candidate& candidate) {
    std::vector<std::string_view> tokens;
    for (const auto id : evaluated.ids_in(candidate.required_use)) {
        tokens.push_back(evaluated.string(id));
    }
    std::set<std::string_view> enabled;
    for (const auto id : evaluated.ids_in(candidate.use)) {
        enabled.insert(evaluated.string(id));
    }
    return check_required_use(tokens, enabled, candidate.empty_groups_true);
}

Plan plan_updates(const Store& store, const Evaluated& evaluated, UseRebuilds rebuilds,
                  const Targets& targets) {
    // emerge restarts with each USE change autounmask asks for, as many times as it backtracks.
    constexpr int restarts = 20;
    std::vector<NeededUseChange> needed;
    std::shared_ptr<const Evaluated> changed;
    const auto changes_of = [&needed] {
        std::vector<UseChange> changes;
        changes.reserve(needed.size());
        for (const auto& each : needed) {
            changes.push_back(each.change);
        }
        return changes;
    };
    for (int round = 0;; ++round) {
        const auto changes = changes_of();
        const auto& current = changed ? *changed : evaluated;
        Planner planner(store, current, rebuilds, targets, changes);
        auto plan = planner.run();
        auto proposed = std::move(planner).proposed();
        if (proposed.empty() || round == restarts) {
            weigh_blockers(store, current, targets, plan);
            replace_slots(store, current, targets.replace_slots, targets.kept_slots, plan);
            order_merges(store, current, plan);
            // Only what the plan merges, as emerge shows only what its graph holds.
            for (auto& each : needed) {
                if (std::ranges::any_of(plan.merges, [&](const Merge& merge) {
                        return merge.candidate == each.change.candidate;
                    })) {
                    plan.use_changes.push_back(std::move(each));
                }
            }
            std::ranges::sort(plan.use_changes, [&](const auto& a, const auto& b) {
                return current.string(current.candidates.at(a.change.candidate).cpv) <
                       current.string(current.candidates.at(b.change.candidate).cpv);
            });
            plan.changed = std::move(changed);
            return plan;
        }
        for (auto& each : proposed) {
            const auto earlier = std::ranges::find_if(needed, [&](const NeededUseChange& had) {
                return had.change.candidate == each.change.candidate;
            });
            if (earlier == needed.end()) {
                needed.push_back(std::move(each));
            } else {
                // Its flags build on the earlier ones; the first reason stays.
                earlier->change.flags = std::move(each.change.flags);
            }
        }
        changed =
            std::make_shared<const Evaluated>(with_use_changes(evaluated, store, changes_of()));
    }
}

} // namespace egraph
