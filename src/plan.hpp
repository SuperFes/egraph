#pragma once

#include "evaluated.hpp"
#include "query.hpp"
#include "required_use.hpp"
#include "store.hpp"
#include "use_changes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace egraph {

// A package the plan holds: installed, or a candidate it merges.
struct Member {
    bool candidate = false;
    // Installed package id, or index into Evaluated::candidates.
    std::uint32_t index = 0;
    auto operator<=>(const Member&) const = default;
};

// A dependency of a member that stands in a merge's way, or pulls one in.
struct Reason {
    Member member;
    // The atom as the member's dependencies print it.
    std::string atom;
    auto operator<=>(const Reason&) const = default;
};

// The dependency kinds by which one merge waits for another.
struct WaitKinds {
    // DEPEND or BDEPEND: merged before it builds.
    bool build = false;
    // IDEPEND: merged before it merges.
    bool install = false;
    // RDEPEND.
    bool run = false;
    // PDEPEND: merged after it where the order allows.
    bool post = false;
    // emerge's implicit wait on a libc it merges, which every later merge has.
    bool libc = false;
    // Reached through installed packages that stay, as emerge's scheduler waits for any merge
    // its graph leads to.
    bool through = false;
    auto operator<=>(const WaitKinds&) const = default;
};

// A merge another merge waits for.
struct Wait {
    // Index into Plan::merges.
    std::uint32_t merge = 0;
    WaitKinds kinds;
    auto operator<=>(const Wait&) const = default;
};

// The wait holds its merge after the other by one of its own dependencies: what the plan's
// displays list, for a merge placed before it.
[[nodiscard]] inline bool ordering(const WaitKinds& kinds) {
    return kinds.build || kinds.install || kinds.run;
}

// The kinds as the plan's table writes them after a place: b build, i install, r run, p post,
// l libc, t through.
[[nodiscard]] std::string wait_letters(const WaitKinds& kinds);

// A package the plan merges.
struct Merge {
    // Index into Evaluated::candidates.
    std::uint32_t candidate = 0;
    // The installed package it replaces, with the update's kind and flags; none for a package
    // new in its slot.
    std::optional<std::uint32_t> replaces;
    UpdateKind kind = UpdateKind::upgrade;
    std::string flags;
    // For a new package, the dependency that first pulled it in, or the argument naming it.
    std::optional<Reason> pulled_by;
    std::optional<Argument> named_by;
    // For a slot-operator rebuild of the installed version: the merge whose slot or sub-slot
    // breaks its binding, with the bound atom as the rebuilt package's dependencies print it.
    std::optional<Reason> rebuilt_for;
    // The other merges its DEPEND, BDEPEND, IDEPEND, RDEPEND or PDEPEND match, every member of a
    // || counting, a libc emerge merges before it, and those any installed package it depends on
    // leads to through others; by merge. Those after it in Plan::order are waits the order
    // breaks: PDEPEND's, a cycle's, and some through installed packages.
    std::vector<Wait> waits;
};

// An installed package whose pending update the plan leaves out, or replaces with an earlier
// version.
struct HeldBack {
    std::uint32_t package = 0;
    PendingUpdate wanted;
    // What rejects it, sorted: dependents' atoms that the update would leave unsatisfied, and
    // the target's own dependencies (or those of what it pulls in) that nothing can satisfy.
    std::vector<Reason> reasons;
};

// A blocker between two packages of what a plan leaves installed.
struct Block {
    // The package whose dependencies hold the blocker, and one it matches.
    Member holder;
    // As the holder's dependencies print it, "!" or "!!" first.
    std::string atom;
    Member blocked;
    auto operator<=>(const Block&) const = default;
};

// An installed package emerge uninstalls to resolve a blocker between it and a merge.
struct Uninstall {
    std::uint32_t package = 0;
    Block why;
};

// A dependency nothing visible satisfies.
struct Missing {
    // The package whose dependency it is; none for one of emerge's arguments.
    std::optional<Member> member;
    // As the member's dependencies print it, or the argument's atom.
    std::string atom;
    auto operator<=>(const Missing&) const = default;
};

// A USE change the plan needs, as autounmask proposes it for package.use.
struct NeededUseChange {
    UseChange change;
    // The first dependency that needed it, or the argument whose atom did.
    std::optional<Reason> pulled_by;
    std::optional<Argument> named_by;
};

struct Plan {
    // Replacements in the installed packages' order, then new packages by cpv.
    std::vector<Merge> merges;
    // Indices into merges, in an order to merge them: each after what it waits for, a run-time
    // dependency's wait dropped where a cycle leaves no other way, then a build-time one's.
    std::vector<std::uint32_t> order;
    // In the installed packages' order.
    std::vector<HeldBack> held;
    // In the installed packages' order, each for the first blocker found to need it.
    std::vector<Uninstall> uninstalls;
    // The blockers emerge cannot resolve, which make it refuse the plan: each with every package
    // it matches that is in the way, sorted.
    std::vector<Block> blocks;
    // Dependencies nothing can satisfy, which make emerge refuse the plan: of what it must merge
    // once every version it could fall back to has failed, each a package's with no visible
    // version left to match, or an argument plain emerge has no visible version of; sorted.
    std::vector<Missing> unsatisfied;
    // Indices into Evaluated::candidates, sorted: versions whose REQUIRED_USE their USE leaves
    // unsatisfied, which make emerge refuse the plan. emerge checks a version as it selects it,
    // before its dependencies, and then backtracks no more: so every version a pass merged or
    // pulled in counts, even one given up later, but for what a pull would add after a
    // dependency nothing satisfies stops emerge first (see plan_updates).
    std::vector<std::uint32_t> unmet;
    // USE changes to merges, by candidate, that emerge's autounmask would ask for; it refuses
    // the plan until they are made.
    std::vector<NeededUseChange> use_changes;
    // The evaluated store the plan was made against, with use_changes made; none without them.
    std::shared_ptr<const Evaluated> changed;

    // emerge would refuse the plan.
    [[nodiscard]] bool refused() const {
        return !blocks.empty() || !unsatisfied.empty() || !unmet.empty() || !use_changes.empty();
    }

    // What the plan's candidate indices mean: changed, or the evaluated store it was made from.
    [[nodiscard]] const Evaluated&
    evaluated_or(const Evaluated& original EGRAPH_LIFETIMEBOUND) const EGRAPH_LIFETIMEBOUND {
        return changed ? *changed : original;
    }
};

// The package.use line emerge asks for the change: ">=cpv flags" when nothing visible or
// installed of its cp is newer, ">=cpv:slot flags" when nothing in its slot is, else "=cpv flags";
// the flags by name, "-flag" turning one off.
[[nodiscard]] std::string package_use_line(const Store& store, const Evaluated& evaluated,
                                           const UseChange& change);

// What needed the change, nearest first, as emerge's "# required by" comments name them: each
// package as cpv::repo, up to "atom (argument)" for an argument of emerge's, or "@set" for a
// set's atom. A package the plan replaces ends the chain.
[[nodiscard]] std::vector<std::string> required_by(const Store& store, const Evaluated& evaluated,
                                                   const Plan& plan, const NeededUseChange& needed);

// The candidate's REQUIRED_USE weighed under its USE.
[[nodiscard]] RequiredUse required_use_of(const Evaluated& evaluated, const Candidate& candidate);

// What emerge -uD would merge for targets, with store's dependencies (read with or without
// dynamic deps) for the installed packages and the candidates' own for what it merges:
// - each package's pending update, or the best visible version in its slot that no member's
//   dependency rejects; held back when none is left;
// - with targets.roots, the best visible version a root atom matches, new in its slot;
// - whatever a member's dependencies need that nothing in the plan satisfies: the best visible
//   version that matches, new in its slot, the first alternative of a || that can be satisfied
//   so. A merge whose own dependencies cannot be satisfied falls back in turn;
// - a kept dependent bound by a slot operator to a sub-slot a merge replaces: rebuilt from a
//   visible ebuild of its version, whose dependencies the plan then satisfies; with none, or
//   outside targets.reach, the binding holds the merge back like any bound;
// - a merge's dependency that only a newer version of an installed package without an update of
//   its own satisfies: that package replaced with the best visible match, rejected in turn with
//   the merge that needed it.
// Without targets.deep, what plain emerge -u would: only emerge's arguments take their pending
// update, and only its target; a rejected one is dropped and a broken binding holds its merge.
// Kept packages' dependencies only reject what a merge takes away from them; what they already
// lack stays missing, as it does with targets.reach for those outside it.
// With targets.request, its atoms are emerge's arguments, picked as targets.selection says: -u
// updates each installed slot an atom matches to the best version the atom accepts, and adds the
// best version's slot; plain emerge merges the best version, installed or not, and -n only when
// nothing emerge can keep matches. What an argument must merge falls back to the other versions
// its atom matches and rebuilds what binds to it, as plain emerge does; an atom named alone
// under -uD keeps its installed version instead. Outside targets.reach, a rebuild takes the best
// version in its slot, for a run-time binding only.
// A version counts as selected, for Plan::unmet, when a pass merges it or pulls it in, but for
// a pull from dependencies emerge never reaches: those of a kind at or after the first (in
// emerge's order RDEPEND, IDEPEND, PDEPEND, DEPEND, BDEPEND) with an atom nothing satisfies, and
// any || group's once one does. Across packages emerge's order is not followed, so this may
// count a version emerge never gets to.
// A dependency with USE dependencies that no visible version meets as it would be built, emerge's
// autounmask meets with the best visible version that can: every flag the dependency sets is in
// its IUSE (or has a default), none it must change is masked or forced by the profile, and no
// change already asked of it contradicts. The plan is then made again with that version's USE
// changed and its dependencies reduced under it (with_use_changes), until no more are needed.
// Then its blockers are weighed as emerge validates them (weigh_blockers), and -u's greedy
// slots leave out an installed slot whose best version and the atom's best block each other.
[[nodiscard]] Plan plan_updates(const Store& store, const Evaluated& evaluated,
                                UseRebuilds rebuilds, const Targets& targets = {});

} // namespace egraph
