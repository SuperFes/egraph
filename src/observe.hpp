#pragma once

// What a run publishes of itself as it goes, as emerge's ObservabilityMonitor does under
// FEATURES="observability".

#include "emerge.hpp"
#include "schedule.hpp"

#include <cstdint>
#include <optional>
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
    // After keep-going goes on, the merges left are the total, as emerge's next pass counts.
    void resumed(const Schedule& schedule);

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

} // namespace egraph
