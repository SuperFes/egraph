#pragma once

// Running emerges, as portage publishes them with FEATURES="observability": one JSON snapshot
// per emerge in ${EPREFIX}/run/portage/emerge-<pid>.json.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph::emerge {

// A build's cgroup counters (FEATURES="cgroup"); whatever the kernel did not report is unset.
struct Resources {
    std::optional<std::uint64_t> cpu_usec{};
    std::optional<std::uint64_t> mem_current{};
    std::optional<std::uint64_t> mem_peak{};
    std::optional<std::uint64_t> io_read_bytes{};
    std::optional<std::uint64_t> io_write_bytes{};
    bool operator==(const Resources&) const = default;
};

enum class TaskKind : std::uint8_t { build, merge };

// One package an emerge is building or merging.
struct Task {
    std::string cpv{};
    // EROOT, and the package's operation: "merge" or "uninstall".
    std::string root{};
    std::string operation{};
    TaskKind kind = TaskKind::build;
    // The ebuild phase, "merge-wait" once built and waiting to merge, or empty before the first.
    std::string phase{};
    // Installed from a binary package rather than built.
    bool binary = false;
    bool merge_wait = false;
    std::optional<std::int64_t> pid{};
    // When the build started, in seconds since the epoch.
    std::optional<double> start_time{};
    // Seconds since the build started, and of the build itself (frozen once it is done).
    std::optional<double> elapsed{};
    std::optional<double> build_elapsed{};
    Resources resources{};
    bool operator==(const Task&) const = default;
};

struct Jobs {
    std::uint64_t running = 0;
    // Unset for --jobs without a limit.
    std::optional<std::uint64_t> max{};
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::uint64_t failed = 0;
    std::uint64_t merge_wait = 0;
    std::uint64_t merges_pending = 0;
    bool operator==(const Jobs&) const = default;
};

struct Snapshot {
    std::int64_t pid = 0;
    // When emerge wrote it, in seconds since the epoch.
    double timestamp = 0;
    Jobs jobs{};
    std::vector<Task> tasks{};
    bool operator==(const Snapshot&) const = default;
};

// A schema 1 snapshot. Fields missing or of the wrong type take their defaults; anything that is
// not a snapshot, or of another schema, is an error.
[[nodiscard]] std::expected<Snapshot, std::string> parse_snapshot(std::string_view text);

// The snapshot as portage's build_snapshot() writes it, keys sorted, without a newline.
[[nodiscard]] std::string snapshot_json(const Snapshot& snapshot);

// Where emerges publish: ${EPREFIX}/run/portage.
[[nodiscard]] std::filesystem::path status_dir(const std::filesystem::path& eprefix);

// Every live emerge's snapshot in dir, by pid. A file is live when the pid in its name is the one
// inside and that process exists under proc; one left behind by an emerge that died is not.
[[nodiscard]] std::vector<Snapshot> read_snapshots(const std::filesystem::path& dir,
                                                   const std::filesystem::path& proc = "/proc");

// A package emerge has yet to merge, from mtimedb's resume mergelist; emerge drops each one once
// it has merged.
struct Pending {
    // "ebuild" or "binary".
    std::string kind{};
    std::string root{};
    std::string cpv{};
};

// ${EPREFIX}/var/cache/edb/mtimedb, where emerge keeps its merge list.
[[nodiscard]] std::filesystem::path mtimedb_path(const std::filesystem::path& eprefix);

// The merge list's packages in merge order; empty when no emerge left one.
[[nodiscard]] std::vector<Pending> parse_mergelist(std::string_view mtimedb);

// For each pending cpv, the others it waits for, as egraph-build --pending writes them.
using Waits = std::map<std::string, std::vector<std::string>, std::less<>>;
[[nodiscard]] std::expected<Waits, std::string> parse_waits(std::string_view text);

// A pending package's place in the merge list's hierarchy.
struct Branch {
    std::string cpv{};
    // 1 at the top.
    std::size_t depth = 1;
    // How many other pending packages it waits for, wherever they sit.
    std::size_t waiting_for = 0;
    // Tree lines, as tui::Row has them: whether it is its parent's last child, and for each
    // level from 1 to depth - 1 whether that level's line continues past it.
    bool last = false;
    std::vector<bool> rails{};
};

// The merge list as a tree in merge order: each package under the one it waits for that merges
// last before it, and at the top what waits for nothing earlier. Waits on packages that merge
// after it are left out, as emerge's own order breaks those cycles.
[[nodiscard]] std::vector<Branch> hierarchy(const std::vector<Pending>& pending,
                                            const Waits& waits);

} // namespace egraph::emerge
