#pragma once

// The build history: a record of each build as it ends, in ${EROOT}/var/lib/egraph/builds.jsonl,
// one JSON object a line, kept to the last builds of each cp.

#include "cgroup.hpp"
#include "exec.hpp"
#include "pressure.hpp"
#include "schedule.hpp"
#include "usage.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace egraph {

// Bumped when a record's fields change meaning; records of another version are kept, unread.
inline constexpr std::int64_t build_record_version = 1;

// The build environment's variables a worker reports, those set.
inline constexpr std::array<std::string_view, 8> build_environment_keys{
    "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "RUSTFLAGS", "MAKEOPTS", "NINJAOPTS"};

// The cps whose installed versions a record keeps as its toolchain.
inline constexpr std::array<std::string_view, 8> toolchain_cps{
    "dev-lang/rust",  "dev-lang/rust-bin",  "llvm-core/clang", "llvm-core/lld",
    "llvm-core/llvm", "sys-devel/binutils", "sys-devel/gcc",   "sys-libs/glibc"};

struct PhaseTime {
    std::string phase{};
    double seconds = 0;
    // The user and system seconds its processes used, where measured.
    std::optional<double> cpu_seconds{};
    bool operator==(const PhaseTime&) const = default;
};

// How busy the system was over a build: the share of CPU time busy, the mean one-minute load,
// and the share of the time some tasks stalled on CPU, memory or IO; whatever /proc did not say
// is unset.
struct SystemShare {
    std::optional<double> cpu_busy{};
    std::optional<double> load{};
    std::optional<double> stall_cpu{};
    std::optional<double> stall_memory{};
    std::optional<double> stall_io{};
    bool operator==(const SystemShare&) const = default;
};

// The share of a build's time its own tasks stalled on CPU, memory or IO, from its cgroup's
// pressure.
struct StallShare {
    std::optional<double> cpu{};
    std::optional<double> memory{};
    std::optional<double> io{};
    bool operator==(const StallShare&) const = default;
};

// steve's jobs and the tokens it had free: as sampled, or their means over a build.
struct JobserverShare {
    double jobs = 0;
    double free = 0;
    bool operator==(const JobserverShare&) const = default;
};

// Where a build's measures came from: its worker's counters and samples of the processes under
// it, or its own cgroup, exact.
enum class Measured : std::uint8_t { proc, cgroup };

// Why a build ran: its package new in its slot, a version up or down from the one it replaces,
// or the same version again, for changed USE, for a slot or sub-slot it binds to, or otherwise
// (asked for, or the installed one masked).
enum class BuildReason : std::uint8_t { added, upgrade, downgrade, use, subslot, reinstall };

// As records and scripts name it: "new", "upgrade", "downgrade", "use", "subslot", "reinstall".
[[nodiscard]] std::string_view build_reason_name(BuildReason reason);
[[nodiscard]] std::optional<BuildReason> parse_build_reason(std::string_view name);

struct BuildRecord {
    enum class Outcome : std::uint8_t { merged, failed };
    // Who ran it: "exec".
    std::string origin{};
    std::string run{};
    // When the build started, in seconds since the epoch.
    double start = 0;
    std::string cpv{};
    std::string repo{};
    std::string slot{};
    // The flags it was built with, sorted.
    std::vector<std::string> use{};
    // The package.env files applied to it, in order.
    std::vector<std::string> env_files{};
    // Of build_environment_keys, those the build had set.
    std::map<std::string, std::string> environment{};
    // By toolchain cp, the versions installed when the build started.
    std::map<std::string, std::vector<std::string>> toolchain{};
    // The build's phases in order, the last one failed's up to the failure.
    std::vector<PhaseTime> phases{};
    double build_seconds = 0;
    // Merged only: the merge's seconds, and how long merge-wait held it after its build.
    std::optional<double> merge_seconds{};
    std::optional<double> waited{};
    Outcome outcome = Outcome::merged;
    // Failed only: the phase that failed ("merge" for the merge), its exit status and build log,
    // or the worker's error.
    std::optional<std::string> failed_phase{};
    std::optional<std::int64_t> status{};
    std::optional<std::string> log{};
    std::optional<std::string> error{};
    // Builds at once the run allowed; none for no limit.
    std::optional<std::uint32_t> jobs{};
    // The most other builds running at once beside it.
    std::uint32_t beside = 0;
    // The user and system seconds of its processes, the most memory they held at once, and the
    // bytes they read from and wrote to storage; from its cgroup where it had one, else from its
    // worker's counters and the memory sampled each second.
    std::optional<double> cpu_seconds{};
    std::optional<std::uint64_t> peak_memory{};
    std::optional<std::uint64_t> read_bytes{};
    std::optional<std::uint64_t> write_bytes{};
    std::optional<Measured> measured{};
    std::optional<SystemShare> system{};
    // Its own stalls, from its cgroup.
    std::optional<StallShare> build_stall{};
    // The bytes its build directory and image took up once it installed, the size of its
    // distfiles, and the size its vdb entry records once merged.
    std::optional<std::uint64_t> build_dir_size{};
    std::optional<std::uint64_t> image_size{};
    std::optional<std::uint64_t> distfiles_size{};
    std::optional<std::uint64_t> installed_size{};
    // Where steve was the jobserver: its jobs and free tokens while it built, on average.
    std::optional<JobserverShare> jobserver{};
    // Why it ran, the installed cpv it replaced, the merge a sub-slot rebuild was for, and a USE
    // rebuild's flags as emerge shows them; unset where not known, as for emerge's builds.
    std::optional<BuildReason> reason{};
    std::optional<std::string> replaced{};
    std::optional<std::string> rebuilt_for{};
    std::optional<std::string> use_changed{};
    bool operator==(const BuildRecord&) const = default;
};

[[nodiscard]] std::filesystem::path builds_path(const std::filesystem::path& eroot);

// Its build's seconds without its fetch phase.
[[nodiscard]] double seconds_building(const BuildRecord& record);

// The record as one line of JSON, without its newline.
[[nodiscard]] std::string build_record_json(const BuildRecord& record);
// A line back; an error for one that does not parse or is of another version.
[[nodiscard]] std::expected<BuildRecord, std::string> parse_build_record(std::string_view line);

// The file's records in order, the lines that do not parse left out.
[[nodiscard]] std::vector<BuildRecord> read_builds(std::string_view text);

// The file's text with line appended (if any), keeping the last keep records of each cp in
// their order; lines that do not parse are kept as they are.
[[nodiscard]] std::string pruned_builds(std::string_view text, std::size_t keep,
                                        std::optional<std::string_view> line = std::nullopt);

// Appends the record to the file at path, pruned to keep a cp, under the directory's lock as
// os::rewrite_locked takes it. keep 0 records nothing.
std::expected<void, std::error_code> append_build(const std::filesystem::path& path,
                                                  const BuildRecord& record, std::size_t keep);

// The records the packages (atoms or cps) match, all when there are none; an error names an
// atom that does not parse.
[[nodiscard]] std::expected<std::vector<BuildRecord>, std::string>
matching_builds(std::vector<BuildRecord> records, std::span<const std::string> packages);

// For scripts, tab-separated: start, cpv, repo, outcome, build seconds, merge seconds (empty
// when it failed), the phase that failed (empty when it merged), jobs (empty for no limit),
// beside, CPU seconds and peak memory (empty where not measured), and why it ran (empty where
// not known).
[[nodiscard]] std::string build_fields(const BuildRecord& record);

// As CSV: a header line, then a row per record.
[[nodiscard]] std::string_view builds_csv_header();
[[nodiscard]] std::string build_csv_row(const BuildRecord& record);

// What a merge step builds, as its record names it.
struct BuildStep {
    std::string cpv{};
    std::string cp{};
    std::string version{};
    std::string repo{};
    std::string slot{};
    std::vector<std::string> use{};
    std::vector<std::string> env_files{};
    bool uninstall = false;
    // A merge's: why it builds, what it replaces, the merge a sub-slot rebuild is for, and a USE
    // rebuild's flags.
    BuildReason reason = BuildReason::added;
    std::optional<std::string> replaced{};
    std::optional<std::string> rebuilt_for{};
    std::optional<std::string> use_changed{};
};

// By toolchain cp, by slot, the version installed.
using Toolchain = std::map<std::string, std::map<std::string, std::string>>;

// What each step builds, as its record names it: a merge's candidate (in the plan's evaluated
// store) with the flags it is built with, sorted, the env files package.env applies to it, and
// why the plan merges it; an uninstall's installed package.
[[nodiscard]] std::vector<BuildStep> build_steps(const Store& store, const Evaluated& evaluated,
                                                 const Plan& plan, std::span<const Step> steps);

// The toolchain cps' versions installed, by slot.
[[nodiscard]] Toolchain installed_toolchain(const Store& store);

// The SIZE the vdb entry of an installed cpv records.
[[nodiscard]] std::optional<std::uint64_t> installed_size(const std::filesystem::path& vdb,
                                                          std::string_view cpv);

// What /proc said at one of a build's events: its worker's counters, and the system's; and its
// cgroup leaf's counters, once its processes made it.
struct Probe {
    std::optional<usage::Counters> worker{};
    std::optional<pressure::Sample> system{};
    std::optional<cgroup::Counters> cgroup{};
};

// The records of a run's builds, from what its schedule traces and its workers report, as
// RunEvents logs them: a merge step's record once it merged or failed, none for an uninstall.
// Probes taken at the build's start, phases and end measure it, and samples taken while it builds
// its memory and the load.
class BuildRecorder {
  public:
    // toolchain: as installed when the run started; merges of its cps change it for the builds
    // that start after them.
    BuildRecorder(std::string origin, std::string run, std::vector<BuildStep> steps,
                  Toolchain toolchain, std::optional<std::uint32_t> jobs);

    [[nodiscard]] std::optional<BuildRecord> traced(std::size_t step, Traced what, double now,
                                                    const Probe& probe = {});
    void reported(std::size_t step, const WorkerEvent& event, double now, const Probe& probe = {});
    // While step builds: the memory its processes hold, the load, and steve's tokens.
    void sampled(std::size_t step, std::uint64_t memory, std::optional<double> load,
                 std::optional<JobserverShare> tokens = std::nullopt);

  private:
    struct Times {
        std::optional<double> build_start;
        std::optional<double> built;
        std::optional<double> merge_start;
        // The build's phase running, and since when.
        std::optional<std::string> phase;
        std::optional<double> phase_start;
        std::vector<PhaseTime> phases;
        std::map<std::string, std::string> environment;
        std::map<std::string, std::vector<std::string>> toolchain;
        std::optional<WorkerEvent> failure;
        std::uint32_t beside = 0;
        bool building = false;
        // The probes at the build's start, its phase's start and its end.
        Probe started;
        Probe phase_started;
        Probe ended;
        std::optional<std::uint64_t> peak_memory;
        // The loads and steve's tokens sampled while it built, summed.
        double loads = 0;
        std::size_t load_samples = 0;
        JobserverShare tokens;
        std::size_t token_samples = 0;
        std::map<std::string, std::uint64_t> sizes;
    };

    static void end_phase(Times& times, double now, const Probe& probe);
    [[nodiscard]] BuildRecord record(std::size_t step, BuildRecord::Outcome outcome) const;

    std::string origin_;
    std::string run_;
    std::vector<BuildStep> steps_;
    Toolchain toolchain_;
    std::optional<std::uint32_t> jobs_;
    std::vector<Times> times_;
};

} // namespace egraph
