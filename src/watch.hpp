#pragma once

// egraph watch: the stores kept fresh as their inputs change, until asked to stop.

#include "store.hpp"

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace egraph {

// The directories whose entries changing can make the inputs stat differently: a directory
// input itself, the directory holding any other. Sorted, each once.
[[nodiscard]] std::vector<std::filesystem::path>
watch_directories(std::span<const std::span<const Input>> layers);

// When a burst of changes has settled enough to refresh for: quiet for a while after the last
// change, or long enough after the first that a steady trickle (a long emerge) still refreshes.
class Debounce {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds quiet{3000};
    static constexpr std::chrono::milliseconds longest{60000};

    void changed(Clock::time_point now);
    [[nodiscard]] bool due(Clock::time_point now) const;
    // How long until due(); none while nothing has changed.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait(Clock::time_point now) const;
    void reset();

  private:
    bool pending_ = false;
    Clock::time_point first_{};
    Clock::time_point last_{};
};

// Keeps the stores fresh until a stop is asked. refresh() brings them up to date and returns the
// directories to watch, or why it could not, which is logged and tried again on the next change
// or after a while, twice as long each time. watch(directories) returns a watcher on them, or why
// not, as std::expected<W, std::string>; its wait(timeout) is os::Watcher::wait's. stale() tells
// whether the stores went stale since the refresh, as a change during one leaves them, which
// counts as a change. now() is the time. The error when no watcher could be opened or waited on.
template <class RefreshFn, class WatchFn, class StaleFn, class NowFn>
std::expected<void, std::string> keep_fresh(RefreshFn refresh, WatchFn watch, StaleFn stale,
                                            NowFn now, std::ostream& log) {
    using std::chrono::milliseconds;
    constexpr milliseconds longest_retry{3'600'000};
    std::vector<std::filesystem::path> directories;
    Debounce debounce;
    // After a failed refresh, when to try again for want of a change.
    bool retrying = false;
    Debounce::Clock::time_point retry{};
    milliseconds backoff = Debounce::longest;
    bool due = true;
    while (true) {
        bool refreshed = false;
        if (due) {
            debounce.reset();
            retrying = false;
            if (auto done = refresh()) {
                directories = std::move(*done);
                backoff = Debounce::longest;
                refreshed = true;
            } else {
                log << "egraph: watch: " << done.error() << '\n';
                retrying = true;
                retry = now() + backoff;
                backoff = std::min(backoff * 2, longest_retry);
            }
            due = false;
        }
        auto watcher = watch(directories);
        if (!watcher) {
            return std::unexpected(std::move(watcher.error()));
        }
        if (refreshed && stale()) {
            debounce.changed(now());
        }
        while (!due) {
            auto timeout = debounce.wait(now());
            if (retrying) {
                const auto left =
                    std::max(milliseconds{0}, std::chrono::ceil<milliseconds>(retry - now()));
                timeout = timeout ? std::min(*timeout, left) : left;
            }
            const auto woken = watcher->wait(timeout);
            if (!woken) {
                return std::unexpected("watch: " + woken.error().message());
            }
            if (woken->stop) {
                return {};
            }
            if (woken->changed) {
                debounce.changed(now());
            }
            due = debounce.due(now()) || (retrying && now() >= retry);
        }
    }
}

} // namespace egraph
