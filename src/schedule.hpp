#pragma once

// When each step of a plan may run beside the others, as emerge's scheduler decides it with
// --jobs and FEATURES=merge-wait.

#include "exec.hpp"
#include "plan.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
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

    // Nothing runs, and nothing more will start.
    [[nodiscard]] bool finished() const;
    // A step failed.
    [[nodiscard]] bool failed() const;

    [[nodiscard]] std::span<const Step> steps() const { return steps_; }

  private:
    enum class State : std::uint8_t { queued, building, built, merging, done, failed };

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
};

} // namespace egraph
