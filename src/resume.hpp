#pragma once

// A plan as emerge records a merge list to resume, the entry "resume" of mtimedb.

#include "evaluated.hpp"
#include "plan.hpp"
#include "verify.hpp"

#include <string>
#include <string_view>

namespace egraph {

// mtimedb's "resume" entry for plan under eroot (EROOT as portage spells it, with its trailing
// slash), as JSON: each merge as ["ebuild", eroot, cpv, "merge"] in the plan's order, the
// request's options as emerge keeps them, and its targets as favorites unless oneshot, which
// emerge selects once merged. Uninstalls have no place in it: emerge weighs the blockers again
// on resuming, and orders the merges again too, but under --nodeps.
[[nodiscard]] std::string resume_entry(const Evaluated& evaluated, const Plan& plan,
                                       std::string_view eroot, const EmergeRequest& request,
                                       bool oneshot);

} // namespace egraph
