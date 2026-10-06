#pragma once

// The events egraph exec logs of a run (log.hpp), from what its schedule traces and its workers
// report: the run, each phase as it starts, each package built, merged or uninstalled with its
// times, failures with their logs, skips, and the end.

#include "exec.hpp"
#include "log.hpp"
#include "schedule.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace egraph {

struct RunStep {
    std::string cpv{};
    bool uninstall = false;
};

struct RunStart {
    std::string command{};
    std::vector<std::string> targets{};
    // As emerge takes them: the request's, --oneshot.
    std::vector<std::string> options{};
    // Builds at once; none for no limit.
    std::optional<std::uint32_t> jobs{};
    bool keep_going = false;
};

class RunEvents {
  public:
    RunEvents(std::string run, std::vector<RunStep> steps);

    [[nodiscard]] log::Event started(const RunStart& start, double now);

    // The event for what the schedule traced of step, if the log keeps one: a build or merge
    // starting has none (its phases follow), nor a skip (skipped says why).
    [[nodiscard]] std::optional<log::Event> traced(std::size_t step, Traced what, double now);

    // The event for what step's worker reported, if the log keeps one: a phase starting. A
    // failure is kept for the trace that follows it.
    [[nodiscard]] std::optional<log::Event> reported(std::size_t step, const WorkerEvent& event,
                                                     double now);

    [[nodiscard]] log::Event skipped(std::size_t step, const std::string& why, double now);

    // The run's end, with what stopped it if anything did.
    [[nodiscard]] log::Event ended(const std::optional<std::string>& error, double now) const;

  private:
    struct Times {
        std::optional<double> build_start;
        std::optional<double> built;
        std::optional<double> merge_start;
        std::optional<WorkerEvent> failure;
    };

    [[nodiscard]] log::Event step_event(std::size_t step, std::string kind, std::string message,
                                        double now) const;

    std::string run_;
    std::vector<RunStep> steps_;
    std::vector<Times> times_;
    std::string command_;
    double start_ = 0;
    std::int64_t merged_ = 0;
    std::int64_t uninstalled_ = 0;
    std::int64_t failed_ = 0;
    std::int64_t skipped_ = 0;
};

// A command handing the system to a program that carries the work out and logs its own steps
// (emerge, emaint): a run of two events, its start with the program's command line, and its end.
struct HandOver {
    std::string command{};
    std::vector<std::string> targets{};
    // "emerge", and the whole command line run.
    std::string program{};
    std::vector<std::string> argv{};
};

[[nodiscard]] log::Event handed_over(const std::string& run, const HandOver& hand_over, double now);

// How the program exited, or why it could not run.
[[nodiscard]] log::Event handed_back(const std::string& run, const HandOver& hand_over,
                                     const std::expected<int, std::string>& ran, double started,
                                     double now);

} // namespace egraph
