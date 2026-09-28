#pragma once

// How hard the system is working, from /proc: CPU time, available memory, load, and pressure
// stall information when the kernel keeps it (CONFIG_PSI, and psi=1 where it is off by default).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string_view>

namespace egraph::pressure {

// Cumulative CPU time over every CPU, in clock ticks.
struct CpuTimes {
    std::uint64_t busy = 0;
    std::uint64_t total = 0;
};

// Some tasks stalled on a resource, as a percentage of the last 10 seconds.
struct Stalls {
    std::optional<double> cpu{};
    std::optional<double> memory{};
    std::optional<double> io{};
};

// One reading of /proc; whatever could not be read is unset.
struct Sample {
    std::optional<CpuTimes> cpu{};
    std::size_t cpus = 0;
    std::optional<std::uint64_t> mem_total{};
    std::optional<std::uint64_t> mem_available{};
    // The one-minute load average.
    std::optional<double> load{};
    Stalls stalls{};
};

// The aggregate "cpu" line of /proc/stat, and how many "cpuN" lines follow it.
[[nodiscard]] std::optional<CpuTimes> parse_stat(std::string_view text);
[[nodiscard]] std::size_t count_cpus(std::string_view text);
// MemTotal and MemAvailable from /proc/meminfo, in bytes.
[[nodiscard]] std::optional<std::uint64_t> parse_meminfo(std::string_view text,
                                                         std::string_view field);
[[nodiscard]] std::optional<double> parse_loadavg(std::string_view text);
// The "some" line's avg10 from a /proc/pressure file.
[[nodiscard]] std::optional<double> parse_psi(std::string_view text);

[[nodiscard]] Sample read_sample(const std::filesystem::path& proc = "/proc");

// The share of CPU time spent busy between two samples, from 0 to 1.
[[nodiscard]] std::optional<double> cpu_busy(const Sample& before, const Sample& after);

// What a reading adds to the graphs.
struct Reading {
    std::optional<double> cpu_busy{};
    std::optional<std::uint64_t> mem_available{};
    std::optional<double> load{};
    Stalls stalls{};
};

// The last readings, oldest first, and the latest sample to measure the next against.
class History {
  public:
    explicit History(std::size_t capacity = 240) : capacity_(capacity) {}
    void add(const Sample& sample);
    [[nodiscard]] const std::deque<Reading>& readings() const { return readings_; }
    [[nodiscard]] const std::optional<Sample>& latest() const { return latest_; }

  private:
    std::size_t capacity_;
    std::deque<Reading> readings_;
    std::optional<Sample> latest_;
};

} // namespace egraph::pressure
