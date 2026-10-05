#include "schedule.hpp"

#include "merge_wait.hpp"

#include <algorithm>
#include <limits>
#include <variant>

namespace egraph {

namespace {

constexpr auto no_step = std::numeric_limits<std::size_t>::max();

} // namespace

Schedule::Schedule(const Plan& plan, std::vector<Step> steps, std::optional<std::uint32_t> jobs,
                   MergeWait merge_wait)
    : plan_{plan}, steps_{std::move(steps)}, jobs_{jobs}, states_(steps_.size(), State::queued),
      step_of_merge_(plan.merges.size(), no_step), merge_wait_{std::move(merge_wait)},
      was_flushed_(steps_.size(), false), unsatisfied_(steps_.size(), false) {
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        if (const auto* merge = std::get_if<MergeStep>(&steps_.at(i))) {
            step_of_merge_.at(merge->merge) = i;
        }
    }
}

bool Schedule::dependent(std::size_t step) const {
    const auto& merges = plan_.get().merges;
    const auto start = std::get<MergeStep>(steps_.at(step)).merge;
    std::vector<bool> seen(merges.size(), false);
    seen.at(start) = true;
    std::vector<std::uint32_t> stack;
    const auto follow = [&](std::uint32_t merge) {
        for (const auto& wait : merges.at(merge).waits) {
            stack.push_back(wait.merge);
        }
    };
    follow(start);
    while (!stack.empty()) {
        const auto merge = stack.back();
        stack.pop_back();
        if (seen.at(merge)) {
            continue;
        }
        seen.at(merge) = true;
        const auto other = step_of_merge_.at(merge);
        const bool later = other != no_step && other > step && states_.at(other) == State::queued;
        if (other != no_step && !finished_with(states_.at(other)) && !later) {
            return true;
        }
        follow(merge);
    }
    return false;
}

std::optional<std::size_t> Schedule::next_build() const {
    // Nor while merge-wait's merges are let through, as emerge holds new jobs then, nor beside
    // a running build while an alone merge's run-time needs are unmerged.
    if (halted_ || flushed_ > 0 || (jobs_ && building_ >= *jobs_) ||
        (building_ > 0 && std::ranges::contains(unsatisfied_, true))) {
        return std::nullopt;
    }
    std::optional<std::size_t> first;
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        if (!std::holds_alternative<MergeStep>(steps_.at(i)) || states_.at(i) != State::queued) {
            continue;
        }
        if (!first) {
            first = i;
        }
        if (!dependent(i)) {
            return i;
        }
    }
    // With nothing else to wait for, the first goes anyway.
    if (building_ == 0 && !merge_running_ && waiting_.empty() && merging_.empty()) {
        return first;
    }
    return std::nullopt;
}

void Schedule::build_started(std::size_t step) {
    states_.at(step) = State::building;
    ++building_;
}

void Schedule::build_finished(std::size_t step, bool succeeded) {
    --building_;
    if (succeeded) {
        states_.at(step) = State::built;
        if (merge_wait_.feature || alone(step)) {
            waiting_.push_back(step);
        } else {
            merging_.push_back(step);
        }
    } else {
        states_.at(step) = State::failed;
        failed_ = true;
        halted_ = true;
    }
    release();
}

std::optional<std::size_t> Schedule::next_merge() const {
    if (merge_running_ || merging_.empty()) {
        return std::nullopt;
    }
    return merging_.front();
}

void Schedule::merge_started(std::size_t step) {
    std::erase(merging_, step);
    states_.at(step) = State::merging;
    merge_running_ = true;
    if (alone(step) && merge_wait_.running_root) {
        for (const auto merge :
             run_time_waits(plan_.get(), std::get<MergeStep>(steps_.at(step)).merge)) {
            const auto other = step_of_merge_.at(merge);
            if (other != no_step && other != step && states_.at(other) != State::done) {
                unsatisfied_.at(other) = true;
            }
        }
    }
}

void Schedule::merge_finished(std::size_t step, bool succeeded) {
    merge_running_ = false;
    if (was_flushed_.at(step)) {
        was_flushed_.at(step) = false;
        --flushed_;
    }
    if (succeeded) {
        unsatisfied_.at(step) = false;
    }
    states_.at(step) = succeeded ? State::done : State::failed;
    failed_ = failed_ || !succeeded;
    halted_ = halted_ || !succeeded;
    release();
}

void Schedule::release() {
    // Uninstalls whose merges are done go ahead of what waits to merge, unless a step failed.
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < steps_.size() && !halted_; ++i) {
        const auto* uninstall = std::get_if<UninstallStep>(&steps_.at(i));
        if (uninstall == nullptr || states_.at(i) != State::queued) {
            continue;
        }
        const auto& after = plan_.get().uninstalls.at(uninstall->uninstall).after;
        if (std::ranges::all_of(after, [this](std::uint32_t merge) {
                const auto step = step_of_merge_.at(merge);
                return step == no_step || finished_with(states_.at(step));
            })) {
            states_.at(i) = State::built;
            ready.push_back(i);
        }
    }
    merging_.insert(merging_.begin(), ready.begin(), ready.end());
    // merge-wait: what built merges once no build runs and nothing else is merging, an alone
    // step only after what was let through before it, and on its own.
    if (building_ == 0 && !merge_running_ && merging_.empty()) {
        while (!waiting_.empty()) {
            const auto step = waiting_.front();
            if (alone(step) && !merging_.empty()) {
                break;
            }
            waiting_.pop_front();
            merging_.push_back(step);
            was_flushed_.at(step) = true;
            ++flushed_;
            if (alone(step)) {
                break;
            }
        }
    }
}

bool Schedule::finished() const {
    return building_ == 0 && !merge_running_ && merging_.empty() && waiting_.empty() &&
           !next_build();
}

Schedule::Stage Schedule::stage(std::size_t step) const {
    switch (states_.at(step)) {
    case State::queued:
        return Stage::queued;
    case State::building:
        return Stage::building;
    case State::built:
        return std::ranges::contains(waiting_, step) ? Stage::waiting : Stage::let_through;
    case State::merging:
        return Stage::merging;
    case State::done:
        return Stage::done;
    case State::failed:
        return Stage::failed;
    case State::skipped:
        return Stage::skipped;
    }
    return Stage::queued;
}

bool Schedule::failed() const {
    return failed_;
}

std::vector<Standing> Schedule::standing() const {
    std::vector<Standing> found;
    found.reserve(states_.size());
    for (const auto state : states_) {
        found.push_back(state == State::done   ? Standing::done
                        : finished_with(state) ? Standing::gone
                                               : Standing::left);
    }
    return found;
}

void Schedule::resume(std::span<const std::size_t> skipped) {
    for (const auto step : skipped) {
        states_.at(step) = State::skipped;
    }
    // A new pass, as emerge's main loop cleans up after each.
    std::ranges::fill(unsatisfied_, false);
    halted_ = false;
    release();
}

} // namespace egraph
