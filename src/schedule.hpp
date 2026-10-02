#pragma once

// When each step of a plan may run beside the others, as emerge's scheduler decides it with
// --jobs and FEATURES=merge-wait.

#include "exec.hpp"
#include "keep_going.hpp"
#include "plan.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace egraph {

// The steps of a run and where each stands. A merge step builds, then merges once allowed;
// an uninstall step only runs. Builds run beside each other, up to jobs; merges and uninstalls
// one at a time, and with merge-wait only once no build runs, as emerge merges by default.
class Schedule {
  public:
    // jobs: how many builds may run at once; none for no limit.
    Schedule(const Plan& plan, std::vector<Step> steps, std::optional<std::uint32_t> jobs);

    // The step whose build may start now, if any: the first not built in the steps' order
    // that reaches no merge yet to finish, through what each waits for (installed packages
    // that stay included), but those after it; or, with nothing else running or waiting, the
    // first. None at the job limit, while what built is let through to merge, or once a step
    // has failed.
    [[nodiscard]] std::optional<std::size_t> next_build() const;
    void build_started(std::size_t step);
    void build_finished(std::size_t step, bool succeeded);

    // The step to merge or uninstall now, if any: one at a time, merges in the order their
    // builds finished, each once no build runs and nothing is merging, and an uninstall once
    // the merges it waits for are done.
    [[nodiscard]] std::optional<std::size_t> next_merge() const;
    void merge_started(std::size_t step);
    void merge_finished(std::size_t step, bool succeeded);

    // Builds running.
    [[nodiscard]] std::uint32_t building() const { return building_; }
    // No build runs and none waits to merge, when emerge starts a build whatever the room.
    [[nodiscard]] bool alone() const { return building_ == 0 && waiting_.empty(); }

    // Nothing runs, and nothing more will start.
    [[nodiscard]] bool finished() const;
    // A step failed.
    [[nodiscard]] bool failed() const;
    // A step failed since the run last went on, so no build starts.
    [[nodiscard]] bool halted() const { return halted_; }
    // Each step's standing, once finished.
    [[nodiscard]] std::vector<Standing> standing() const;
    // Goes on after a failure, as emerge --keep-going does once what ran has finished: skipped
    // steps never run, and what failed or was skipped holds nothing back.
    void resume(std::span<const std::size_t> skipped);

    [[nodiscard]] std::span<const Step> steps() const { return steps_; }
    [[nodiscard]] const Step& step(std::size_t index) const { return steps_.at(index); }

  private:
    enum class State : std::uint8_t { queued, building, built, merging, done, failed, skipped };

    // Done, failed or skipped: holding nothing back.
    [[nodiscard]] static bool finished_with(State state) {
        return state == State::done || state == State::failed || state == State::skipped;
    }
    // Whether step's build reaches, through what merges wait for, a merge yet to finish that
    // is not queued after it.
    [[nodiscard]] bool dependent(std::size_t step) const;
    // Lets through what may merge next.
    void release();

    std::reference_wrapper<const Plan> plan_;
    std::vector<Step> steps_;
    std::optional<std::uint32_t> jobs_;
    std::vector<State> states_;
    // By merge index, its step.
    std::vector<std::size_t> step_of_merge_;
    // Built steps waiting to merge, in the order their builds finished.
    std::deque<std::size_t> waiting_;
    // Steps let through to merge, one at a time.
    std::deque<std::size_t> merging_;
    // Merge steps let through from waiting_ and not yet merged.
    std::size_t flushed_ = 0;
    std::uint32_t building_ = 0;
    bool merge_running_ = false;
    bool failed_ = false;
    bool halted_ = false;
};

// What a run reports of its steps as they go, beside each worker event.
enum class Traced : std::uint8_t {
    build_started,
    built,
    build_failed,
    merge_started,
    merged,
    merge_failed,
    skipped
};

// How a run over a pool went.
struct PoolOutcome {
    struct Stop {
        // The step it stopped at, if one did.
        std::optional<std::size_t> step;
        std::string why;
    };
    // The steps merged or uninstalled.
    std::size_t done = 0;
    // The first failure: what the worker reported, or what went wrong talking to it.
    std::optional<Stop> stopped;
    // Under keep-going: the failures after it, and the steps skipped for them.
    std::vector<Stop> also_failed;
    std::vector<std::size_t> skipped;
};

// What Pool::next() heard.
struct Heard {
    // The worker heard from; none for a token perhaps freed.
    std::optional<std::size_t> worker;
    // Its line; none once it has ended.
    std::optional<std::string> line;
};

// Runs the schedule's steps over a pool of workers, each running one request at a time: a build
// on any idle worker once the schedule lets it start, there is room for it (or it would run
// alone, as emerge's _can_add_job weighs PORTAGE_TMPDIR's free space) and a token is had, its
// merge on the worker that built it, an uninstall on any idle worker; a worker is added when none
// is idle. Calls report(step, event) for each event a worker reports and trace(step, what) as each
// step goes; after a failure, what runs still finishes and what built still merges, as the schedule
// says, and then resume(schedule) -> std::optional<std::vector<std::size_t>> names the steps to
// skip as the run goes on, or none to end it, as emerge --keep-going decides. Pool: add() ->
// std::expected<std::size_t, std::string>, the new worker's index;
//   send(worker, line) -> bool; take_token(step) -> std::expected<bool, std::string>, false when
//   none is free yet; give_token(step) -> std::expected<void, std::string>; next(for_token) ->
//   std::expected<Heard, std::string>, waiting for a line or end from a worker, or with for_token,
//   a token perhaps freed; room_for(running) -> bool, whether a build may start beside those
//   running.
// Requests: build(step), merge(step) -> std::string, written as each is sent; done(step).
template <class Pool, class Requests, class Report, class Trace, class Resume>
PoolOutcome run_schedule(Schedule& schedule, Pool& pool, Requests& requests, const Report& report,
                         const Trace& trace, const Resume& resume) {
    struct Running {
        std::size_t step = 0;
        bool merging = false;
    };
    struct Worker {
        std::optional<Running> running;
        bool ended = false;
    };
    constexpr auto nobody = std::numeric_limits<std::size_t>::max();
    PoolOutcome outcome;
    std::vector<Worker> workers;
    // By step, the worker that built it.
    std::vector<std::size_t> builder(schedule.steps().size(), nobody);
    const auto stop = [&](std::optional<std::size_t> step, std::string why) {
        PoolOutcome::Stop failure{.step = step, .why = std::move(why)};
        if (!outcome.stopped) {
            outcome.stopped = std::move(failure);
        } else {
            outcome.also_failed.push_back(std::move(failure));
        }
    };
    const auto idle = [&]() -> std::expected<std::size_t, std::string> {
        for (std::size_t worker = 0; worker < workers.size(); ++worker) {
            if (!workers.at(worker).ended && !workers.at(worker).running) {
                return worker;
            }
        }
        auto added = pool.add();
        if (added) {
            workers.resize(std::max(workers.size(), *added + 1));
        }
        return added;
    };
    const auto finish = [&](std::size_t worker, bool succeeded, std::string why) {
        const auto [step, merging] = *workers.at(worker).running;
        workers.at(worker).running.reset();
        if (merging) {
            schedule.merge_finished(step, succeeded);
            trace(step, succeeded ? Traced::merged : Traced::merge_failed);
            if (succeeded) {
                requests.done(step);
                ++outcome.done;
            }
        } else {
            if (auto given = pool.give_token(step); !given && succeeded) {
                succeeded = false;
                why = std::move(given.error());
            }
            schedule.build_finished(step, succeeded);
            trace(step, succeeded ? Traced::built : Traced::build_failed);
        }
        if (!succeeded) {
            stop(step, std::move(why));
        }
    };
    for (;;) {
        bool started = true;
        while (started) {
            started = false;
            const auto step = schedule.next_merge();
            if (!step) {
                break;
            }
            std::optional<std::size_t> worker;
            if (std::holds_alternative<MergeStep>(schedule.step(*step))) {
                const auto built_by = builder.at(*step);
                if (workers.at(built_by).ended) {
                    schedule.merge_started(*step);
                    schedule.merge_finished(*step, false);
                    trace(*step, Traced::merge_failed);
                    stop(step, "the worker that built it has ended");
                    started = true;
                    continue;
                }
                if (!workers.at(built_by).running) {
                    worker = built_by;
                }
            } else if (auto found = idle()) {
                worker = *found;
            } else {
                schedule.merge_started(*step);
                schedule.merge_finished(*step, false);
                stop(step, std::move(found.error()));
                started = true;
                continue;
            }
            if (!worker) {
                break;
            }
            schedule.merge_started(*step);
            workers.at(*worker).running = Running{.step = *step, .merging = true};
            trace(*step, Traced::merge_started);
            if (!pool.send(*worker, requests.merge(*step))) {
                workers.at(*worker).ended = true;
                finish(*worker, false, "the worker takes no more requests");
                started = true;
            }
        }
        bool for_token = false;
        while (const auto step = schedule.next_build()) {
            if (!schedule.alone() && !pool.room_for(schedule.building())) {
                break;
            }
            const auto token = pool.take_token(*step);
            if (token && !*token) {
                for_token = true;
                break;
            }
            const auto worker = token ? idle() : std::expected<std::size_t, std::string>{};
            if (!token || !worker) {
                if (token) {
                    std::ignore = pool.give_token(*step);
                }
                schedule.build_started(*step);
                schedule.build_finished(*step, false);
                stop(step, token ? worker.error() : token.error());
                break;
            }
            schedule.build_started(*step);
            builder.at(*step) = *worker;
            workers.at(*worker).running = Running{.step = *step, .merging = false};
            trace(*step, Traced::build_started);
            if (!pool.send(*worker, requests.build(*step))) {
                workers.at(*worker).ended = true;
                finish(*worker, false, "the worker takes no more requests");
            }
        }
        if (schedule.finished()) {
            if (!schedule.halted() || std::ranges::none_of(schedule.standing(), [](Standing each) {
                    return each == Standing::left;
                })) {
                return outcome;
            }
            const auto skipped = resume(std::as_const(schedule));
            if (!skipped) {
                return outcome;
            }
            for (const auto step : *skipped) {
                trace(step, Traced::skipped);
            }
            outcome.skipped.insert(outcome.skipped.end(), skipped->begin(), skipped->end());
            schedule.resume(*skipped);
            continue;
        }
        if (!for_token &&
            std::ranges::none_of(workers, [](const Worker& w) { return w.running.has_value(); })) {
            stop(std::nullopt, "nothing can run, though the run is not done");
            return outcome;
        }
        const auto heard = pool.next(for_token);
        if (!heard) {
            stop(std::nullopt, heard.error());
            return outcome;
        }
        if (!heard->worker) {
            continue;
        }
        const auto worker = *heard->worker;
        if (!heard->line) {
            workers.at(worker).ended = true;
            if (workers.at(worker).running) {
                finish(worker, false, "the worker ended before the request was done");
            }
            continue;
        }
        const auto running = workers.at(worker).running;
        if (!running) {
            continue;
        }
        const auto step = running->step;
        const auto event = parse_event(*heard->line);
        if (!event) {
            workers.at(worker).ended = true;
            finish(worker, false, "the worker reported what egraph cannot read: " + event.error());
            continue;
        }
        report(step, *event);
        if (!final_event(*event)) {
            continue;
        }
        const auto kind = event->kind;
        const bool succeeded = running->merging ? kind == WorkerEvent::Kind::merged ||
                                                      kind == WorkerEvent::Kind::uninstalled
                                                : kind == WorkerEvent::Kind::built;
        finish(worker, succeeded, succeeded ? std::string{} : describe_event(*event));
    }
}

} // namespace egraph
