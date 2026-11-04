#pragma once

// What exec shows of its run. Off a terminal, a line for each status as it arrives. On one, the
// packages being worked on stay in a region at the bottom, a line each updated in place, with
// the run's totals beneath; a package that is done leaves it and its last line stays above.

#include <cstddef>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace egraph {

class RunDisplay {
  public:
    struct Options {
        // The region redraws in place; else every status is a line of its own.
        bool live = false;
        // Plain lines as "(i of n) name: what" for people, else tab-separated for scripts.
        bool human = true;
        // The columns a line may take; a live line is cut to fit, never wrapped.
        std::size_t width = 80;
        std::size_t total = 0;
    };

    RunDisplay(std::ostream& out, Options options) : out_(out), options_(options) {}

    // A package's status changes, and it stays in the region.
    void status(std::size_t step, std::string_view name, std::string_view what);
    // A package is done, with its last status, which a failure marks.
    void finish(std::size_t step, std::string_view name, std::string_view what, bool failed);
    // A line of the run's own, above the region.
    void note(std::string_view line);
    // The totals line: seconds since the start, and the estimate of those left when known.
    void tick(double elapsed, std::optional<double> remaining);
    // Leaves the region's last state in place, and the cursor under it.
    void close();

    [[nodiscard]] std::size_t done() const { return done_; }
    [[nodiscard]] std::size_t failures() const { return failures_; }

  private:
    [[nodiscard]] std::string line_of(std::size_t step, std::string_view name,
                                      std::string_view what) const;
    [[nodiscard]] std::string totals() const;
    [[nodiscard]] std::string cut(std::string_view text) const;
    // Replaces the region with the lines above it, then the region as it stands.
    void redraw(std::string_view above);

    std::ostream& out_;
    Options options_;
    // The packages being worked on, in plan order, with the line each shows.
    std::map<std::size_t, std::string> active_{};
    std::size_t drawn_ = 0;
    std::size_t done_ = 0;
    std::size_t failures_ = 0;
    double elapsed_ = 0;
    std::optional<double> remaining_{};
};

// "1 h 05 min", "4 min 07 s" or "9 s": a run's time as the totals line gives it.
[[nodiscard]] std::string run_duration(double seconds);

} // namespace egraph
