#pragma once

// What a process and those under it use, from /proc: the CPU time and storage IO a worker's
// finished children add to its own counters, and the memory of the processes running under it.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace egraph::usage {

// A process's counters, its children's included once it has waited for them.
struct Counters {
    // User and system seconds of the children it waited for, not its own.
    double children_cpu = 0;
    // Bytes read from and written to storage, its own and its children's; unset where the kernel
    // does not account IO (CONFIG_TASK_IO_ACCOUNTING) or will not say.
    std::optional<std::uint64_t> read_bytes{};
    std::optional<std::uint64_t> write_bytes{};
    bool operator==(const Counters&) const = default;
};

// From /proc/<pid>/stat: the parent, the resident pages, and the waited-for children's ticks.
struct Stat {
    std::int64_t ppid = 0;
    std::uint64_t rss_pages = 0;
    std::uint64_t children_ticks = 0;
    bool operator==(const Stat&) const = default;
};

// Bytes from /proc/<pid>/io.
struct Io {
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    bool operator==(const Io&) const = default;
};

[[nodiscard]] std::optional<Stat> parse_stat(std::string_view text);
[[nodiscard]] std::optional<Io> parse_io(std::string_view text);

// The process's counters, none once it has ended; ticks: clock ticks a second.
[[nodiscard]] std::optional<Counters> read_counters(std::int64_t pid, std::uint64_t ticks,
                                                    const std::filesystem::path& proc = "/proc");

// A process running, as a reading of /proc saw it.
struct Process {
    std::int64_t pid = 0;
    std::int64_t ppid = 0;
    std::uint64_t rss_pages = 0;
    bool operator==(const Process&) const = default;
};

// Every process /proc shows.
[[nodiscard]] std::vector<Process> read_processes(const std::filesystem::path& proc = "/proc");

// The resident pages of the processes under pid, at any depth, pid's own left out.
[[nodiscard]] std::uint64_t pages_under(const std::vector<Process>& processes, std::int64_t pid);

} // namespace egraph::usage
