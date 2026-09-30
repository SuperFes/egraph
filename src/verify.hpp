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

// A merge list: the merges and uninstalls, and the blockers emerge cannot resolve.
struct Pretend {
    std::vector<PretendMerge> merges;
    std::vector<PretendBlock> blocks;
    bool operator==(const Pretend&) const = default;
};

// What emerge --pretend --verbose --color=n printed: its merges in its order, each uninstall as
// one of kind uninstall; and the blockers it could not resolve, sorted. Everything else it
// printed (resolved blockers, messages) passed over.
[[nodiscard]] Pretend parse_pretend(std::string_view output);

// What plan merges and uninstalls, and its blocks, in the same terms; only a new package has
// its USE.
[[nodiscard]] Pretend planned_merges(const Store& store, const Evaluated& evaluated,
                                     const Plan& plan);

// Where ours and emerge's merge lists differ, by cpv then repo, a line each:
// "cpv::repo<TAB>egraph<TAB>kind" merged by egraph only, "cpv::repo<TAB>emerge<TAB>kind" by emerge
// only, "cpv::repo<TAB>kind<TAB>ours<TAB>emerge's", and for a package both merge new,
// "cpv::repo<TAB>use<TAB>ours<TAB>emerge's". A blocker only one side cannot resolve is
// "holder<TAB>egraph<TAB>blocks atom" or "holder<TAB>emerge<TAB>blocks atom".
[[nodiscard]] std::vector<std::string> merge_differences(const Pretend& ours,
                                                         const Pretend& theirs);

} // namespace egraph
