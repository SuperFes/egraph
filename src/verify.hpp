#pragma once

// A plan held to emerge's own: the same request through emerge --pretend, the merge lists
// compared.

#include "evaluated.hpp"
#include "plan.hpp"
#include "query.hpp"
#include "store.hpp"

#include <compare>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// A request as emerge's options and arguments put it.
struct EmergeRequest {
    // Atoms and sets.
    std::vector<std::string> targets;
    bool update = false;
    bool deep = false;
    bool noreplace = false;
    UseRebuilds rebuilds = UseRebuilds::none;
    bool dynamic_deps = true;
};

// emerge's options and arguments for request, pretending and verbose, without the options in
// EMERGE_DEFAULT_OPTS, which would change what it prints or plans.
[[nodiscard]] std::vector<std::string> pretend_arguments(const EmergeRequest& request);

// One package of a merge list.
struct PretendMerge {
    std::string cpv;
    std::string repo;
    // new (in its slot or not), upgrade, downgrade or rebuild.
    std::string kind;
    // Its USE as emerge --verbose shows it: a group per USE_EXPAND variable shown, NAME="...".
    std::string use;
    auto operator<=>(const PretendMerge&) const = default;
};

// A blocker emerge cannot resolve, once per package holding it.
struct PretendBlock {
    // Without its "!"s, as emerge shows it.
    std::string atom;
    std::string holder;
    auto operator<=>(const PretendBlock&) const = default;
};

// A merge list: the merges and uninstalls, the blockers emerge cannot resolve, and the atoms of
// dependencies nothing satisfies.
struct Pretend {
    std::vector<PretendMerge> merges;
    std::vector<PretendBlock> blocks;
    std::vector<std::string> unsatisfied;
    // "cpv::repo" of each version whose REQUIRED_USE is unmet.
    std::vector<std::string> unmet;
    // "cpv flags" for each USE change autounmask asks for, the flags by name.
    std::vector<std::string> use_changes;
    bool operator==(const Pretend&) const = default;
};

// What emerge --pretend --verbose --color=n printed: its merges in its order, each uninstall as
// one of kind uninstall; the blockers it could not resolve, sorted; and the atoms it found no
// ebuild, or only masked ones, to satisfy, sorted, when it failed: it names them for the updates
// it skips too; the versions it found REQUIRED_USE unmet for, sorted; and the USE changes it asks
// for, sorted. Everything else it printed (resolved blockers, messages) passed over.
[[nodiscard]] Pretend parse_pretend(std::string_view output, bool failed);

// What plan merges and uninstalls, its blocks, its unsatisfied dependencies, its unmet
// REQUIRED_USE and the USE changes it needs, in the same terms; only a new package has its USE.
[[nodiscard]] Pretend planned_merges(const Store& store, const Evaluated& evaluated,
                                     const Plan& plan);

// Where ours and emerge's merge lists differ, by cpv then repo, a line each:
// "cpv::repo<TAB>egraph<TAB>kind" merged by egraph only, "cpv::repo<TAB>emerge<TAB>kind" by emerge
// only, "cpv::repo<TAB>kind<TAB>ours<TAB>emerge's", and for a package both merge new,
// "cpv::repo<TAB>use<TAB>ours<TAB>emerge's". A blocker only one side cannot resolve is
// "holder<TAB>egraph<TAB>blocks atom" or "holder<TAB>emerge<TAB>blocks atom". emerge refusing a
// plan for dependencies nothing satisfies, or REQUIRED_USE unmet, names what it found first, and
// prints no merge list: then only each atom or version it names that ours lacks differs,
// "atom<TAB>emerge<TAB>unsatisfied" or "cpv::repo<TAB>emerge<TAB>required-use"; when ours alone
// refuses, each of its own does, "atom<TAB>egraph<TAB>unsatisfied" or
// "cpv::repo<TAB>egraph<TAB>required-use". emerge asking for USE changes prints a merge list made
// before some of them (upstream-notes.md): then only the changes differ, a change one side alone
// asks for "cpv<TAB>emerge<TAB>use-change flags" or "cpv<TAB>egraph<TAB>use-change flags".
[[nodiscard]] std::vector<std::string> merge_differences(const Pretend& ours,
                                                         const Pretend& theirs);

} // namespace egraph
