#pragma once

// A plan as emerge records a merge list to resume, the entry "resume" of mtimedb.

#include "evaluated.hpp"
#include "plan.hpp"
#include "schedule.hpp"
#include "verify.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// emerge's options as its parser leaves them, the "myopts" of the resume entry, as a JSON
// object: options are each --name or --name=value, as egraph passes them to emerge.
[[nodiscard]] std::string emerge_myopts(std::span<const std::string> options);

// mtimedb's "resume" entry for plan under eroot (EROOT as portage spells it, with its trailing
// slash), as JSON: each merge as ["ebuild", eroot, cpv, "merge"] in the plan's order, the
// request's options as emerge keeps them, and its targets as favorites unless oneshot, which
// emerge selects once merged. Uninstalls have no place in it: emerge weighs the blockers again
// on resuming, and orders the merges again too, but under --nodeps.
[[nodiscard]] std::string resume_entry(const Evaluated& evaluated, const Plan& plan,
                                       std::string_view eroot, const EmergeRequest& request,
                                       bool oneshot);

// The same for the plan's merges listed (indices into Plan::merges, in their order), emerge's
// options as emerge_myopts takes them, and favorites.
[[nodiscard]] std::string resume_entry(const Evaluated& evaluated, const Plan& plan,
                                       std::string_view eroot, std::span<const std::size_t> listed,
                                       std::span<const std::string> options,
                                       std::span<const std::string> favorites);

// The merge steps the resume entry lists through a run, as emerge's scheduler keeps it: every
// one at the start, each dropped as it merges, the failed ones once --keep-going weighs them, and
// the list saved again when the run goes on. What --keep-going drops without saving, emerge
// commits only at its end, and only once the run has changed what is installed.
class ResumeList {
  public:
    explicit ResumeList(const Schedule& schedule);

    // After a pass under --keep-going, with the steps it skips as the run goes on, none when it
    // ends: whether to save the list now.
    [[nodiscard]] bool weighed(const Schedule& schedule,
                               std::optional<std::span<const std::size_t>> skipped);

    // At the run's end: whether to save the list now.
    [[nodiscard]] bool ended(const Schedule& schedule, bool keep_going);

    // The merge steps listed and not merged yet, in step order.
    [[nodiscard]] std::vector<std::size_t> steps(const Schedule& schedule) const;

  private:
    void drop_failed(const Schedule& schedule);

    std::vector<bool> listed_;
    // Dropped but not saved.
    bool unsaved_ = false;
};

} // namespace egraph
