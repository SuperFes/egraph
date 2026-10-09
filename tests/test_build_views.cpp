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

namespace {

BuildRecord with(BuildRecord record, std::vector<std::string> use,
                 std::optional<egraph::BuildReason> reason = std::nullopt) {
    record.use = std::move(use);
    record.reason = reason;
    return record;
}

} // namespace

TEST_CASE("what rebuilds most, by why: its records' reasons, else inferred from the build before") {
    using egraph::BuildReason;
    auto subslot = with(built("a/x-2", 4, 1), {"gtk"}, BuildReason::subslot);
    subslot.rebuilt_for = "dev-libs/icu-77";
    auto again = with(built("a/x-2", 5, 1), {}, BuildReason::subslot);
    again.rebuilt_for = "dev-libs/icu-78.1";
    auto other_slot = built("a/x-3", 7, 1);
    other_slot.slot = "3";
    const std::vector<BuildRecord> records{
        with(built("a/x-1", 1, 1), {}, BuildReason::added),
        with(built("a/x-1", 2, 1), {"gtk"}), // the same version, other flags: USE
        with(built("a/x-2", 3, 1), {"gtk"}), // upgrade
        subslot,
        again,
        with(built("a/y-1", 5, 1), {}),                        // nothing before it: not known
        with(built("a/x-1", 6, 1), {}),                        // downgrade
        with(built("a/y-1", 6, 1), {}),                        // the same again: reinstall
        other_slot,                                            // nothing before it in its slot
        with(built("a/z-1", 8, 1), {}, BuildReason::upgrade)}; // built once
    const auto found = egraph::frequent_rebuilds(records);
    REQUIRE(found.size() == 2);
    const auto& x = found.at(0);
    CHECK(x.cp == "a/x");
    CHECK(x.builds == 7);
    CHECK(x.reasons == std::array<std::size_t, 6>{1, 1, 1, 1, 2, 0});
    CHECK(x.unknown == 1);
    CHECK(x.flags == std::vector<egraph::Tally>{{.name = "gtk", .count = 2}});
    CHECK(x.causes == std::vector<egraph::Tally>{{.name = "dev-libs/icu", .count = 2}});
    const auto& y = found.at(1);
    CHECK(y.cp == "a/y");
    CHECK(y.reasons == std::array<std::size_t, 6>{0, 0, 0, 0, 0, 1});
    CHECK(y.unknown == 1);
    CHECK(y.flags.empty());

    CHECK(egraph::rebuild_fields(x) == "a/x\t7\t1\t1\t1\t1\t2\t0\t1\tgtk:2\tdev-libs/icu:2");
    CHECK(egraph::rebuild_fields(y) == "a/y\t2\t0\t0\t0\t0\t0\t1\t1\t\t");
}

TEST_CASE("what fails: each cp's failures, the phases, and whether it merged since") {
    using Outcome = BuildRecord::Outcome;
    auto installing = built("p/f-2", 3, 1, Outcome::failed);
    installing.failed_phase = "install";
    installing.log = "/var/tmp/portage/p/f-2/temp/build.log";
    auto refused = built("p/g-1", 4, 1, Outcome::failed);
    refused.failed_phase.reset();
    refused.error = "no ebuild\tof p/g-1";
    const std::vector<BuildRecord> records{built("p/f-1", 1, 1, Outcome::failed),
                                           built("p/f-1", 2, 1),
                                           installing,
                                           refused,
                                           built("p/g-2", 5, 1),
                                           built("p/h-1", 6, 1)};
    const auto found = egraph::failing_packages(records);
    REQUIRE(found.size() == 2);
    const auto& g = found.at(0);
    CHECK(g.cp == "p/g");
    CHECK(g.builds == 2);
    CHECK(g.failed == 1);
    CHECK(g.phases == std::vector<egraph::Tally>{{.name = "worker", .count = 1}});
    // Another version merged: not the same one passing.
    CHECK(g.passed_later == 0);
    CHECK(g.merged_since);
    CHECK(g.last_log == "no ebuild\tof p/g-1");
    const auto& f = found.at(1);
    CHECK(f.builds == 3);
    CHECK(f.failed == 2);
    CHECK(f.phases == std::vector<egraph::Tally>{{.name = "compile", .count = 1},
                                                 {.name = "install", .count = 1}});
    CHECK(f.passed_later == 1);
    CHECK_FALSE(f.merged_since);
    CHECK(f.last_start == 3);
    CHECK(f.last_cpv == "p/f-2");

    CHECK(egraph::failures_fields(f) == "p/f\t3\t2\t1\tfailing\t3\tp/f-2\tcompile:1 install:1\t"
                                        "/var/tmp/portage/p/f-2/temp/build.log");
    // A tab in an error would split its field.
    CHECK(egraph::failures_fields(g) ==
          "p/g\t2\t1\t0\tmerged\t4\tp/g-1\tworker:1\tno ebuild of p/g-1");
}

TEST_CASE("what changed between two builds: version, toolchain, environment, env files, USE") {
    auto before = with(built("a/x-1", 1, 1), {"doc", "x"});
    before.toolchain = {{"sys-devel/gcc", {"15.2.1"}}, {"sys-devel/binutils", {"2.45"}}};
    before.environment = {{"CFLAGS", "-O2"}, {"LDFLAGS", "-Wl,-O1"}};
    auto after = with(built("a/x-2", 2, 1), {"lto", "x"});
    after.toolchain = {{"sys-devel/gcc", {"15.2.1", "16.2.1"}},
                       {"sys-devel/binutils", {"2.45"}},
                       {"sys-libs/glibc", {"2.42"}}};
    after.environment = {{"CFLAGS", "-O2 -flto"}};
    after.env_files = {"lto.conf"};
    CHECK(egraph::build_changes(before, after) ==
          std::vector<egraph::BuildChange>{
              {.what = "version", .before = "1", .after = "2"},
              {.what = "sys-devel/gcc", .before = "15.2.1", .after = "15.2.1 16.2.1"},
              {.what = "sys-libs/glibc", .before = "", .after = "2.42"},
              {.what = "CFLAGS", .before = "-O2", .after = "-O2 -flto"},
              {.what = "LDFLAGS", .before = "-Wl,-O1", .after = ""},
              {.what = "package.env", .before = "", .after = "lto.conf"},
              {.what = "USE", .before = "doc", .after = "lto"}});
    CHECK(egraph::build_changes(before, before).empty());
}

TEST_CASE("a latest build slower than those before it, by CPU time where it was measured") {
    using Outcome = BuildRecord::Outcome;
    const auto cpu = [](BuildRecord record, double seconds) {
        record.cpu_seconds = seconds;
        return record;
    };
    const auto beside = [](BuildRecord record, std::uint32_t others) {
        record.beside = others;
        return record;
    };
    auto previous = cpu(built("a/s-1", 4, 50), 105);
    previous.toolchain = {{"sys-devel/gcc", {"15"}}};
    auto latest = cpu(built("a/s-2", 5, 50), 200);
    latest.toolchain = {{"sys-devel/gcc", {"16"}}};
    const std::vector<BuildRecord> records{
        // Measured: its CPU time against the median of those before, the wall time aside.
        cpu(built("a/s-1", 0, 900), 300), // more than 5 before it: left out
        cpu(built("a/s-1", 1, 50), 100), cpu(built("a/s-1", 2, 50), 110),
        cpu(built("a/s-1", 3, 50), 90), cpu(built("a/s-1", 3, 50), 105), previous, latest,
        // A failure after it changes nothing.
        built("a/s-3", 6, 1, Outcome::failed),
        // Not measured: only builds alone weigh.
        built("a/u-1", 1, 100), beside(built("a/u-1", 2, 300), 2), built("a/u-1", 3, 200),
        // One weighed build without CPU time: wall time for all.
        cpu(built("a/v-1", 1, 50), 10), built("a/v-1", 2, 50), cpu(built("a/v-1", 3, 100), 1000),
        // Slower, but not by enough.
        built("a/t-1", 1, 100), built("a/t-1", 2, 100), built("a/t-1", 3, 120),
        // Twice as slow, but by seconds only.
        cpu(built("a/w-1", 1, 1), 10), cpu(built("a/w-1", 2, 1), 20),
        // Its latest beside others, unmeasured: nothing to weigh.
        built("a/r-1", 1, 10), beside(built("a/r-1", 2, 900), 1),
        // Built once.
        cpu(built("a/q-1", 1, 1), 1000)};
    const auto found = egraph::slower_builds(records);
    REQUIRE(found.size() == 3);
    CHECK(found.at(0).cp == "a/u");
    CHECK_FALSE(found.at(0).cpu);
    CHECK(found.at(0).seconds == 200);
    CHECK(found.at(0).before == 100);
    CHECK(found.at(1).cp == "a/v");
    CHECK_FALSE(found.at(1).cpu);
    CHECK(found.at(1).before == 50);
    const auto& s = found.at(2);
    CHECK(s.cp == "a/s");
    CHECK(s.cpv == "a/s-2");
    CHECK(s.start == 5);
    CHECK(s.cpu);
    CHECK(s.seconds == 200);
    CHECK(s.before == 105);
    CHECK(s.changes == std::vector<egraph::BuildChange>{
                           {.what = "version", .before = "1", .after = "2"},
                           {.what = "sys-devel/gcc", .before = "15", .after = "16"}});
    CHECK(egraph::slowdown_lines(s) == std::vector<std::string>{"a/s\ta/s-2\t5\tcpu\t200.0\t105.0",
                                                                "\tchange\tversion\t1\t2",
                                                                "\tchange\tsys-devel/gcc\t15\t16"});
}
