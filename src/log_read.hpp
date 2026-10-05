#pragma once

// egraph's logged runs read back (log.hpp), from its file or from the journal as journalctl
// prints it, and summed up a run a line for egraph log.

#include "log.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph::log {

// The events of the file's lines, skipping a line that is not one.
[[nodiscard]] std::vector<Event> file_events(std::string_view text);

// The events of journalctl -o json's lines, egraph's alone: their fields as strings, as the
// journal keeps them, their time the journal's.
[[nodiscard]] std::vector<Event> journal_events(std::string_view text);

// The journalctl command line that prints egraph's entries.
[[nodiscard]] std::vector<std::string> journal_command();

struct RunSummary {
    std::string run{};
    // When it started, seconds since the epoch.
    double started = 0;
    std::string command{};
    std::string targets{};
    // done, failed, or unfinished without an end: cut short, or running still.
    std::string status{};
    std::int64_t merged = 0;
    std::int64_t uninstalled = 0;
    std::int64_t failed = 0;
    std::int64_t skipped = 0;
    std::optional<double> seconds{};
};

// Each run the events hold, in the order of its first event.
[[nodiscard]] std::vector<RunSummary> summarize(std::span<const Event> events);

// The run whose id is or starts with prefix; an error for none or more than one.
[[nodiscard]] std::expected<std::string, std::string> find_run(std::span<const RunSummary> runs,
                                                               std::string_view prefix);

// For people, in zone: "2026-10-05 12:03:11  <run>  exec app-misc/a  done: 2 merged, 0
// uninstalled, 0 failed, 0 skipped in 2 min 5 s".
[[nodiscard]] std::string summary_line(const RunSummary& run, const std::chrono::time_zone& zone);

// For scripts, tab-separated: run, start, command, status, merged, uninstalled, failed, skipped,
// seconds (empty when unfinished) and targets.
[[nodiscard]] std::string summary_fields(const RunSummary& run);

// An event for people, in zone: its time and message.
[[nodiscard]] std::string event_line(const Event& event, const std::chrono::time_zone& zone);

} // namespace egraph::log
