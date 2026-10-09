#pragma once

// emerge's builds as its FEATURES="observability" snapshots show them, for the build history:
// each build followed from snapshot to snapshot, its phases timed as they change, its cgroup
// counters as the last snapshot froze them, and its outcome settled by its vdb entry.

#include "builds.hpp"
#include "emerge.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace egraph {

// A build emerge ran, as far as its snapshots showed it, once it is gone from them.
struct EmergeBuild {
    // emerge's.
    std::int64_t pid = 0;
    std::string cpv{};
    std::string root{};
    double start = 0;
    // As the snapshots saw the phase change; one already running when first seen starts with the
    // build.
    std::vector<PhaseTime> phases{};
    // When its build ended, as emerge froze its time, and when its merge began.
    std::optional<double> built{};
    std::optional<double> merge_start{};
    // When it was found gone.
    double ended = 0;
    emerge::Resources resources{};
    std::optional<std::uint64_t> jobs{};
    std::uint32_t beside = 0;
    // Whether a snapshot showed it merging, and whether emerge was still running once it was
    // gone, as it is after a failure; an emerge gone too leaves its outcome to the vdb.
    bool merging = false;
    bool emerge_running = false;
    bool operator==(const EmergeBuild&) const = default;
};

// Follows the builds in emerge's snapshots, from one reading to the next.
class EmergeWatch {
  public:
    // The emerges publishing now; the builds gone since the last reading.
    [[nodiscard]] std::vector<EmergeBuild> observe(const std::vector<emerge::Snapshot>& live,
                                                   double now);
    // Whether any build is being followed.
    [[nodiscard]] bool following() const { return !builds_.empty(); }

    // A build being followed, and its phase running since when.
    struct Following {
        EmergeBuild build;
        std::optional<std::string> phase;
        double phase_start = 0;
        bool waited = false;
    };

  private:
    std::map<std::pair<std::int64_t, std::string>, Following> builds_;
};

// What an installed package's vdb entry says of its build.
struct VdbEntry {
    std::string repo{};
    std::string slot{};
    // The flags it was built with, of its IUSE, sorted.
    std::vector<std::string> use{};
    // Of build_environment_keys, those recorded, as portage keeps CFLAGS and the like.
    std::map<std::string, std::string> environment{};
    std::optional<double> build_time{};
    std::optional<std::uint64_t> size{};
    bool operator==(const VdbEntry&) const = default;
};

// The entry in vdb for cpv, none when it is not installed.
[[nodiscard]] std::optional<VdbEntry> read_vdb_entry(const std::filesystem::path& vdb,
                                                     const std::string& cpv);

// The build's record, origin "emerge": merged when its vdb entry was built since it started,
// else failed while emerge went on running (in its last phase, or "merge"); none for one whose
// emerge stopped with it, interrupted.
[[nodiscard]] std::optional<BuildRecord> emerge_record(const EmergeBuild& build,
                                                       const std::optional<VdbEntry>& entry,
                                                       const Toolchain& toolchain);

} // namespace egraph
