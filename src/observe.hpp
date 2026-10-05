#pragma once

// What a run publishes of itself as it goes, as emerge's ObservabilityMonitor does under
// FEATURES="observability".

#include "emerge.hpp"
#include "schedule.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// What a status display names a step by.
struct Observed {
    std::string cpv;
    // EROOT.
    std::string root;
    // "merge" or "uninstall".
    std::string operation;
};

// Each step's times, phase and worker, as the snapshot emerge's scheduler publishes reports
// them: a step starts once it leaves the queue (a build starting, an uninstall let through to
// run), and a merge step's build ends once it no longer builds.
class Observer {
  public:
    // pid: the run's process; jobs: the build limit, none for none. steps: as the schedule's.
    Observer(std::int64_t pid, std::optional<std::uint32_t> jobs, std::vector<Observed> steps);

    // Notes, at now, the steps that have left the queue or stopped building since the last.
    void update(const Schedule& schedule, double now);
    // The phase the step's worker reports starting.
    void phase(std::size_t step, std::string name);
    // The worker running the step.
    void worker(std::size_t step, std::int64_t pid);
    // When keep-going goes on without the steps skipped, the merges left are the total, as
    // emerge's next pass counts.
    void resumed(const Schedule& schedule, std::span<const std::size_t> skipped);

    [[nodiscard]] emerge::Snapshot snapshot(const Schedule& schedule, double now) const;

  private:
    struct Times {
        std::optional<double> start;
        std::optional<double> built;
        std::string phase;
        std::optional<std::int64_t> pid;
    };

    std::int64_t pid_;
    std::optional<std::uint32_t> jobs_;
    std::vector<Observed> steps_;
    std::vector<Times> times_;
    std::uint64_t total_ = 0;
};

// The status file a run publishes its snapshots to, as emerge's ObservabilityMonitor writes
// ${EPREFIX}/run/portage/emerge-<pid>.json: replaced whole, and removed with this object.
class StatusFile {
  public:
    // Why it cannot be written is said once on notes, as the run goes on without it.
    StatusFile(std::filesystem::path path, std::ostream& notes EGRAPH_KEPT_BY_THIS);
    StatusFile(const StatusFile&) = delete;
    StatusFile& operator=(const StatusFile&) = delete;
    StatusFile(StatusFile&&) = delete;
    StatusFile& operator=(StatusFile&&) = delete;
    ~StatusFile();

    // Writes the snapshot: for an event, unless the last event wrote one less than a second
    // before; for a tick, always.
    void publish(const emerge::Snapshot& snapshot, bool tick);

  private:
    std::filesystem::path path_;
    std::reference_wrapper<std::ostream> notes_;
    std::optional<double> last_;
    bool written_ = false;
    bool told_ = false;
};

} // namespace egraph
