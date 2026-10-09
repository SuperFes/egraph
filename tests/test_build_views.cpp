#include "build_views.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using egraph::BuildPeriod;
using egraph::BuildRecord;
using egraph::PackageTimes;
using egraph::PeriodTotal;

namespace {

BuildRecord built(std::string cpv, double start, double seconds,
                  BuildRecord::Outcome outcome = BuildRecord::Outcome::merged) {
    BuildRecord record;
    record.origin = "exec";
    record.run = "run1";
    record.start = start;
    record.cpv = std::move(cpv);
    record.repo = "gentoo";
    record.slot = "0";
    record.build_seconds = seconds;
    record.outcome = outcome;
    if (outcome == BuildRecord::Outcome::merged) {
        record.merge_seconds = 2;
    } else {
        record.failed_phase = "compile";
    }
    return record;
}

std::vector<std::string> cps(const std::vector<PackageTimes>& found) {
    std::vector<std::string> names;
    for (const auto& each : found) {
        names.push_back(each.cp);
    }
    return names;
}

std::chrono::year_month_day day(int year, unsigned month, unsigned day) {
    return std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day};
}

} // namespace

TEST_CASE("the slowest packages are those whose merged builds took longest, by their median") {
    using Outcome = BuildRecord::Outcome;
    auto fetched = built("app-misc/x-1", 1, 100);
    // Its fetch is left out, as the estimates leave it.
    fetched.phases = {{.phase = "fetch", .seconds = 20}, {.phase = "compile", .seconds = 80}};
    auto measured = built("app-misc/y-1", 2, 200);
    measured.cpu_seconds = 400;
    measured.peak_memory = 1000;
    auto again = built("app-misc/y-2", 6, 220);
    again.peak_memory = 3000;
    const std::vector<BuildRecord> records{fetched,
                                           measured,
                                           built("app-misc/x-2", 3, 60),
                                           built("app-misc/x-2", 4, 5, Outcome::failed),
                                           built("app-misc/z-1", 5, 500, Outcome::failed),
                                           built("app-misc/w-1", 5, 60),
                                           built("app-misc/v-1", 5, 60),
                                           again};
    const auto found = egraph::slowest_packages(records);
    // Ties by cp; one that never merged has no time.
    CHECK(cps(found) ==
          std::vector<std::string>{"app-misc/y", "app-misc/x", "app-misc/v", "app-misc/w"});
    const auto& y = found.at(0);
    CHECK(y.seconds == std::vector<double>{200, 220});
    CHECK(y.median == 210);
    CHECK(y.cpu == 400);
    CHECK(y.peak_memory == 3000);
    CHECK(y.failed == 0);
    const auto& x = found.at(1);
    CHECK(x.seconds == std::vector<double>{80, 60});
    CHECK(x.median == 70);
    CHECK(x.failed == 1);
    CHECK_FALSE(x.cpu);
    CHECK_FALSE(x.peak_memory);
    CHECK(egraph::slowest_packages({}).empty());

    CHECK(egraph::package_times_fields(y) == "app-misc/y\t2\t0\t210.0\t400.0\t3000\t200.0 220.0");
    CHECK(egraph::package_times_fields(x) == "app-misc/x\t2\t1\t70.0\t\t\t80.0 60.0");
}

TEST_CASE("the time spent building is summed by week from Monday, or by month, local time") {
    const auto* utc = std::chrono::locate_zone("UTC");
    auto measured = built("app-misc/a-1", 1759744800, 100); // Monday 2025-10-06
    measured.cpu_seconds = 300;
    const std::vector<BuildRecord> records{
        measured,
        built("app-misc/b-1", 1759924800, 50, BuildRecord::Outcome::failed), // Wednesday
        built("app-misc/c-1", 1761037200, 10),                               // 2025-10-21
    };
    CHECK(egraph::period_totals(records, BuildPeriod::week, *utc) ==
          std::vector<PeriodTotal>{
              {.start = day(2025, 10, 6), .builds = 2, .failed = 1, .seconds = 152, .cpu = 300},
              // A week without builds stays in its place.
              {.start = day(2025, 10, 13), .builds = 0, .failed = 0, .seconds = 0, .cpu = {}},
              {.start = day(2025, 10, 20), .builds = 1, .failed = 0, .seconds = 12, .cpu = {}}});

    auto september = built("app-misc/d-1", 1759219200, 30); // 2025-09-30
    CHECK(egraph::period_totals(std::vector<BuildRecord>{september, records.at(2)},
                                BuildPeriod::month, *utc) ==
          std::vector<PeriodTotal>{
              {.start = day(2025, 9, 1), .builds = 1, .failed = 0, .seconds = 32, .cpu = {}},
              {.start = day(2025, 10, 1), .builds = 1, .failed = 0, .seconds = 12, .cpu = {}}});

    // Sunday 23:30 in UTC is Monday in Berlin.
    const auto sunday = built("app-misc/e-1", 1759707000, 1);
    CHECK(egraph::period_totals(std::vector<BuildRecord>{sunday}, BuildPeriod::week, *utc)
              .at(0)
              .start == day(2025, 9, 29));
    CHECK(egraph::period_totals(std::vector<BuildRecord>{sunday}, BuildPeriod::week,
                                *std::chrono::locate_zone("Europe/Berlin"))
              .at(0)
              .start == day(2025, 10, 6));
    CHECK(egraph::period_totals({}, BuildPeriod::month, *utc).empty());
}

TEST_CASE("for scripts, a period is its first day, builds, failed, seconds and CPU seconds") {
    CHECK(egraph::period_fields(
              {.start = day(2025, 10, 6), .builds = 2, .failed = 1, .seconds = 152, .cpu = 300}) ==
          "2025-10-06\t2\t1\t152.0\t300.0");
    CHECK(egraph::period_fields({.start = day(2025, 10, 13)}) == "2025-10-13\t0\t0\t0.0\t");
}
