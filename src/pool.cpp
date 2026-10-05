#include "pool.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>

namespace egraph {

WorkerPool::WorkerPool(std::vector<std::string> argv, std::optional<os::Jobserver> jobserver,
                       bool implicit_token, BuildRoom room, std::ostream& notes)
    : argv_{std::move(argv)}, room_{std::move(room)}, notes_{notes},
      jobserver_{std::move(jobserver)}, implicit_token_{implicit_token} {}

bool WorkerPool::room_for(std::uint32_t running) {
    if (room_.free_gb == 0) {
        return true;
    }
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    const auto directory = room_.tmpdir / "portage";
    const auto free = os::free_bytes(directory);
    if (!free) {
        // emerge says so and starts the build.
        return true;
    }
    constexpr auto most = std::numeric_limits<std::uint64_t>::max() / gib;
    const auto wanted = std::min(room_.free_gb, most - running) + running;
    if (*free >= wanted * gib) {
        return true;
    }
    if (!told_room_) {
        told_room_ = true;
        notes_.get() << std::format(
            "{} has {} GiB free, {} GiB wanted for another build: builds wait to run alone, as "
            "emerge's --jobs-tmpdir-require-free-gb has them\n",
            directory.string(), *free / gib, wanted);
    }
    return false;
}

std::expected<std::size_t, std::string> WorkerPool::add() {
    auto talk = os::start_talking(argv_);
    if (!talk) {
        return std::unexpected(talk.error().message);
    }
    talks_.push_back(std::move(*talk));
    indices_.push_back(added_);
    return added_++;
}

bool WorkerPool::send(std::size_t worker, std::string_view line) {
    const auto found = std::ranges::find(indices_, worker);
    if (found == indices_.end()) {
        return false;
    }
    return talks_.at(static_cast<std::size_t>(found - indices_.begin())).send(line);
}

std::expected<bool, std::string> WorkerPool::take_token(std::size_t step) {
    if (!jobserver_) {
        return true;
    }
    if (implicit_token_) {
        implicit_token_ = false;
        tokens_.insert_or_assign(step, std::nullopt);
        return true;
    }
    const auto token = jobserver_->take();
    if (!token) {
        jobserver_.reset();
        return std::unexpected(std::format("the jobserver failed: {}", token.error().message()));
    }
    if (!*token) {
        return false;
    }
    tokens_.insert_or_assign(step, *token);
    return true;
}

std::expected<void, std::string> WorkerPool::give_token(std::size_t step) {
    const auto found = tokens_.find(step);
    if (found == tokens_.end()) {
        return {};
    }
    const auto token = found->second;
    tokens_.erase(found);
    if (!token) {
        implicit_token_ = true;
        return {};
    }
    if (!jobserver_) {
        return {};
    }
    if (const auto given = jobserver_->give(*token); !given) {
        jobserver_.reset();
        return std::unexpected(std::format("the jobserver failed: {}", given.error().message()));
    }
    return {};
}

std::expected<Heard, std::string> WorkerPool::next(bool for_token) {
    while (heard_.empty()) {
        const auto found = os::wait_for(
            talks_, for_token && jobserver_ ? std::optional{std::cref(*jobserver_)} : std::nullopt,
            period_);
        if (!found) {
            return std::unexpected(
                std::format("waiting for the workers: {}", found.error().message()));
        }
        if (found->timed_out) {
            tick_();
            continue;
        }
        if (found->talks.empty() && !found->token) {
            return std::unexpected("there are no workers to hear from");
        }
        std::vector<std::size_t> gone;
        for (const auto talk : found->talks) {
            auto line = talks_.at(talk).receive();
            if (!line) {
                gone.push_back(talk);
            }
            heard_.push_back({.worker = indices_.at(talk), .line = std::move(line)});
        }
        // Erased from the back, so the positions before stay put.
        for (const auto talk : std::views::reverse(gone)) {
            record(talks_.at(talk).finish());
            talks_.erase(talks_.begin() + static_cast<std::ptrdiff_t>(talk));
            indices_.erase(indices_.begin() + static_cast<std::ptrdiff_t>(talk));
        }
        if (found->token) {
            heard_.push_back({.worker = std::nullopt, .line = std::nullopt});
        }
    }
    auto next = std::move(heard_.front());
    heard_.pop_front();
    return next;
}

void WorkerPool::every(std::chrono::milliseconds period, std::function<void()> tick) {
    period_ = period;
    tick_ = std::move(tick);
}

std::optional<std::int64_t> WorkerPool::pid(std::size_t worker) const {
    const auto found = std::ranges::find(indices_, worker);
    if (found == indices_.end()) {
        return std::nullopt;
    }
    return talks_.at(static_cast<std::size_t>(found - indices_.begin())).pid();
}

std::expected<int, std::string> WorkerPool::finish() {
    for (auto& talk : talks_) {
        record(talk.finish());
    }
    talks_.clear();
    indices_.clear();
    return ended_.value_or(0);
}

void WorkerPool::record(const std::expected<int, os::SpawnError>& ended) {
    if (ended_ && (!*ended_ || **ended_ != 0)) {
        return;
    }
    if (ended) {
        ended_ = *ended;
    } else {
        ended_ = std::unexpected(ended.error().message);
    }
}

} // namespace egraph
