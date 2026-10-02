#pragma once

// A plan carried out one step at a time, as egraph-build --worker takes each.

#include "evaluated.hpp"
#include "plan.hpp"
#include "query.hpp"
#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// What emerge's scheduler still counts installed as a run goes, for the blockers a merge takes as
// it merges: what an uninstall removed or a merge replaced (by cpv or slot) is gone, and nothing
// the run merged counts.
class InstalledBlockers {
  public:
    // evaluated: the plan's own (Plan::evaluated_or).
    InstalledBlockers(const Store& store EGRAPH_KEPT_BY_THIS,
                      const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
                      const Plan& plan EGRAPH_KEPT_BY_THIS);

    // Installed package ids, sorted: those still present that the merge blocks or that block it,
    // at run time, but for its own slot and cpv, as BlockerDB.findInstalledBlockers finds them.
    [[nodiscard]] std::vector<std::uint32_t> blockers(std::uint32_t merge) const;
    void merged(std::uint32_t merge);
    void uninstalled(std::uint32_t package);
    // The step merged or uninstalled.
    void done(const Step& step);

  private:
    [[nodiscard]] const Store& store() const { return store_ref_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_ref_.get(); }
    [[nodiscard]] const Candidate& candidate(std::uint32_t merge) const;
    // One of pkg's run-time blockers matches candidate.
    [[nodiscard]] bool blocks(const Package& pkg, const Candidate& candidate,
                              std::string_view cp) const;

    std::reference_wrapper<const Store> store_ref_;
    std::reference_wrapper<const Evaluated> evaluated_ref_;
    std::reference_wrapper<const Plan> plan_ref_;
    // Per installed package, whether emerge's scheduler still counts it installed.
    std::vector<bool> present_;
};

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

// The worker's requests for a run's steps when they run apart, a merge step's build and then its
// merge: the merge's blockers as emerge's scheduler finds them when it merges, after the steps
// done by then.
class StepRequests {
  public:
    StepRequests(const Store& store EGRAPH_KEPT_BY_THIS,
                 const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
                 const Plan& plan EGRAPH_KEPT_BY_THIS, std::vector<Step> steps);

    // A merge step's build, {"build": cpv, "repo": repo}.
    [[nodiscard]] std::string build(std::size_t step) const;
    // A merge step's merge, {"merge": cpv} with its blockers and world atom, or an uninstall
    // step's request.
    [[nodiscard]] std::string merge(std::size_t step) const;
    // The step merged or uninstalled.
    void done(std::size_t step);

  private:
    std::reference_wrapper<const Store> store_ref_;
    std::reference_wrapper<const Evaluated> evaluated_ref_;
    std::reference_wrapper<const Plan> plan_ref_;
    std::vector<Step> steps_;
    InstalledBlockers blockers_;
};

// The cpv the step merges or uninstalls.
[[nodiscard]] std::string step_cpv(const Store& store, const Evaluated& evaluated, const Plan& plan,
                                   const Step& step);

// What egraph-build --worker reports of a request.
struct WorkerEvent {
    enum class Kind : std::uint8_t { phase, built, merged, uninstalled, failed, error };
    Kind kind = Kind::phase;
    // The phase starting, the cpv built or done, the phase that failed, or the error's
    // message.
    std::string text;
    // For a failed phase: its exit status and build log.
    int status = 0;
    std::string log;
    auto operator<=>(const WorkerEvent&) const = default;
};

// The worker's line as an event; an error for anything else.
[[nodiscard]] std::expected<WorkerEvent, std::string> parse_event(std::string_view line);

// Whether the event ends its request: built, done, failed or refused.
[[nodiscard]] bool final_event(const WorkerEvent& event);

// The event as a person reads it, after the step it belongs to.
[[nodiscard]] std::string describe_event(const WorkerEvent& event);

// How a run of requests went.
struct RunOutcome {
    // The requests done, from the first.
    std::size_t done = 0;
    // Why the run stopped short of the rest: the failure the worker reported, or what went
    // wrong talking to it. None when every request was done.
    std::optional<std::string> stopped;
};

// Sends the requests to worker one at a time, each once the one before is done, and calls
// report(index, event) for each event it reports; stops at the first request that is not done.
// Worker: send(line) -> bool, receive() -> std::optional<std::string>, as os::Talk.
template <class Worker, class Report>
RunOutcome run_requests(Worker& worker, std::span<const std::string> requests,
                        const Report& report) {
    RunOutcome outcome;
    std::size_t index = 0;
    for (const auto& request : requests) {
        if (!worker.send(request)) {
            outcome.stopped = "the worker takes no more requests";
            return outcome;
        }
        for (;;) {
            const auto line = worker.receive();
            if (!line) {
                outcome.stopped = "the worker ended before the request was done";
                return outcome;
            }
            const auto event = parse_event(*line);
            if (!event) {
                outcome.stopped = "the worker reported what egraph cannot read: " + event.error();
                return outcome;
            }
            report(index, *event);
            if (!final_event(*event)) {
                continue;
            }
            if (event->kind != WorkerEvent::Kind::merged &&
                event->kind != WorkerEvent::Kind::uninstalled) {
                outcome.stopped = describe_event(*event);
                return outcome;
            }
            break;
        }
        ++outcome.done;
        ++index;
    }
    return outcome;
}

} // namespace egraph
