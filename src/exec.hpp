#pragma once

// A plan carried out one step at a time, as egraph-build --worker takes each.

#include "evaluated.hpp"
#include "plan.hpp"
#include "query.hpp"
#include "store.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace egraph {

struct MergeStep {
    // Index into Plan::merges.
    std::uint32_t merge = 0;
    // Installed package ids, sorted: those still installed when it merges that it blocks or that
    // block it, which it may take files over from.
    std::vector<std::uint32_t> blockers;
    // The atom to record in the world file once merged.
    std::optional<std::string> world;
    auto operator<=>(const MergeStep&) const = default;
};

struct UninstallStep {
    // Index into Plan::uninstalls.
    std::uint32_t uninstall = 0;
    // The world file drops the atoms that then match nothing installed.
    bool clean_world = false;
    auto operator<=>(const UninstallStep&) const = default;
};

using Step = std::variant<MergeStep, UninstallStep>;

// The atom emerge records in the world file for candidate, merged for arguments (emerge's atom
// arguments; sets record nothing), as its create_world_atom works it out before the run: none
// unless an argument matches it; the argument's cp, or its slot atom when the cp is slotted and
// the argument names one slot; none when the world file's best atom for it is that already, or
// when it is unslotted and @system has it outside virtual/. With the argument's repository.
[[nodiscard]] std::optional<std::string> world_atom(const Store& store, const Evaluated& evaluated,
                                                    const Candidate& candidate,
                                                    std::span<const Argument> arguments);

// The plan's steps in the order one worker runs them: its merges in Plan::order, each uninstall
// straight after the last merge it waits for. A merge's blockers are as emerge's scheduler finds
// them as it merges: the run-time blockers between it and what is installed, but for its own
// slot and cpv, where what any earlier step uninstalled or replaced (by cpv or slot) counts
// as gone and nothing the run merged counts yet. Unless oneshot, a merge records its world atom
// and an uninstall an argument matches cleans the world file, as emerge's scheduler does.
[[nodiscard]] std::vector<Step> run_steps(const Store& store, const Evaluated& evaluated,
                                          const Plan& plan, std::span<const Argument> arguments,
                                          bool oneshot);

// The step as egraph-build --worker's request, one line of JSON without its newline.
[[nodiscard]] std::string worker_request(const Store& store, const Evaluated& evaluated,
                                         const Plan& plan, const Step& step);

} // namespace egraph
