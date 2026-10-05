#pragma once

// A plan as emerge records a merge list to resume, the entry "resume" of mtimedb.

#include "evaluated.hpp"
#include "plan.hpp"
#include "verify.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// emerge's options as its parser leaves them, the "myopts" of the resume entry, as a JSON
// object: options are each --name or --name=value, as egraph passes them to emerge.
[[nodiscard]] std::string emerge_myopts(std::span<const std::string> options);

// The favorites emerge records for a request's targets, as parse_request reads them into
// arguments: each set named, and each atom as its argument spells it. In no order, as emerge
// gathers them in a set.
[[nodiscard]] std::vector<std::string> resume_favorites(std::span<const std::string> targets,
                                                        std::span<const Argument> arguments);

// mtimedb's "resume" entry for plan under eroot (EROOT as portage spells it, with its trailing
// slash), as JSON: each merge as ["ebuild", eroot, cpv, "merge"] in the plan's order, the
// request's options as emerge keeps them, and its favorites (resume_favorites), which emerge
// selects once merged unless oneshot. Uninstalls have no place in it: emerge weighs the blockers
// again on resuming, and orders the merges again too, but under --nodeps.
[[nodiscard]] std::string resume_entry(const Evaluated& evaluated, const Plan& plan,
                                       std::string_view eroot, const EmergeRequest& request,
                                       bool oneshot, std::span<const Argument> arguments);

} // namespace egraph
