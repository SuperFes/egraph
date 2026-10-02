#include "schedule.hpp"

#include <algorithm>
#include <limits>
#include <variant>

namespace egraph {

namespace {

constexpr auto no_step = std::numeric_limits<std::size_t>::max();

} // namespace

Schedule::Schedule(const Plan& plan, std::vector<Step> steps, std::optional<std::uint32_t> jobs)
    : plan_{plan}, steps_{std::move(steps)}, jobs_{jobs}, states_(steps_.size(), State::queued),
      step_of_merge_(plan.merges.size(), no_step) {
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
        if (other != no_step && states_.at(other) != State::done && !later) {
            return true;
        }
        follow(merge);
    }
    return false;
}

std::optional<std::size_t> Schedule::next_build() const {
    // Nor while merge-wait's merges are let through, as emerge holds new jobs then.
    if (failed_ || flushed_ > 0 || (jobs_ && building_ >= *jobs_)) {
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
        waiting_.push_back(step);
    } else {
        states_.at(step) = State::failed;
        failed_ = true;
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
}

void Schedule::merge_finished(std::size_t step, bool succeeded) {
    merge_running_ = false;
    if (std::holds_alternative<MergeStep>(steps_.at(step))) {
        --flushed_;
    }
    states_.at(step) = succeeded ? State::done : State::failed;
    failed_ = failed_ || !succeeded;
    release();
}

void Schedule::release() {
    // Uninstalls whose merges are done go ahead of what waits to merge, unless a step failed.
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < steps_.size() && !failed_; ++i) {
        const auto* uninstall = std::get_if<UninstallStep>(&steps_.at(i));
        if (uninstall == nullptr || states_.at(i) != State::queued) {
            continue;
        }
        const auto& after = plan_.get().uninstalls.at(uninstall->uninstall).after;
        if (std::ranges::all_of(after, [this](std::uint32_t merge) {
                const auto step = step_of_merge_.at(merge);
                return step == no_step || states_.at(step) == State::done;
            })) {
            states_.at(i) = State::built;
            ready.push_back(i);
        }
    }
    merging_.insert(merging_.begin(), ready.begin(), ready.end());
    // merge-wait: what built merges once no build runs and nothing else is merging.
    if (building_ == 0 && !merge_running_ && merging_.empty()) {
        flushed_ += waiting_.size();
        merging_.insert(merging_.end(), waiting_.begin(), waiting_.end());
        waiting_.clear();
    }
}

bool Schedule::finished() const {
    return building_ == 0 && !merge_running_ && merging_.empty() && waiting_.empty() &&
           !next_build();
}

bool Schedule::failed() const {
    return failed_;
}

} // namespace egraph
