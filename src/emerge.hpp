#pragma once

// Running emerges, as portage publishes them with FEATURES="observability": one JSON snapshot
// per emerge in ${EPREFIX}/run/portage/emerge-<pid>.json.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph::emerge {

// A build's cgroup counters (FEATURES="cgroup"); whatever the kernel did not report is unset.
struct Resources {
    std::optional<std::uint64_t> cpu_usec;
    std::optional<std::uint64_t> mem_current;
    std::optional<std::uint64_t> mem_peak;
    std::optional<std::uint64_t> io_read_bytes;
    std::optional<std::uint64_t> io_write_bytes;
};

enum class TaskKind : std::uint8_t { build, merge };

// One package an emerge is building or merging.
struct Task {
    std::string cpv;
    TaskKind kind = TaskKind::build;
    // The ebuild phase, "merge-wait" once built and waiting to merge, or empty before the first.
    std::string phase;
    // Installed from a binary package rather than built.
    bool binary = false;
    bool merge_wait = false;
    std::optional<std::int64_t> pid;
    // Seconds since the build started, and of the build itself (frozen once it is done).
    std::optional<double> elapsed;
    std::optional<double> build_elapsed;
    Resources resources;
};

struct Jobs {
    std::uint64_t running = 0;
    // Unset for --jobs without a limit.
    std::optional<std::uint64_t> max;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t failed = 0;
    std::uint64_t merge_wait = 0;
    std::uint64_t merges_pending = 0;
};

struct Snapshot {
    std::int64_t pid = 0;
    // When emerge wrote it, in seconds since the epoch.
    double timestamp = 0;
    Jobs jobs;
    std::vector<Task> tasks;
};

// A schema 1 snapshot. Fields missing or of the wrong type take their defaults; anything that is
// not a snapshot, or of another schema, is an error.
[[nodiscard]] std::expected<Snapshot, std::string> parse_snapshot(std::string_view text);

// Where emerges publish: ${EPREFIX}/run/portage.
[[nodiscard]] std::filesystem::path status_dir(const std::filesystem::path& eprefix);

// Every live emerge's snapshot in dir, by pid. A file is live when the pid in its name is the one
// inside and that process exists under proc; one left behind by an emerge that died is not.
[[nodiscard]] std::vector<Snapshot> read_snapshots(const std::filesystem::path& dir,
                                                   const std::filesystem::path& proc = "/proc");

} // namespace egraph::emerge
