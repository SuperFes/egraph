#include "emerge_builds.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using egraph::EmergeBuild;
using egraph::EmergeWatch;
using egraph::PhaseTime;
using egraph::emerge::Snapshot;
using egraph::emerge::Task;
using egraph::emerge::TaskKind;

namespace {

Task building(std::string cpv, std::string phase, double start) {
    return {.cpv = std::move(cpv),
            .root = "/",
            .operation = "merge",
            .kind = TaskKind::build,
            .phase = std::move(phase),
            .start_time = start};
}

Snapshot at(double timestamp, std::vector<Task> tasks) {
    return {.pid = 50,
            .timestamp = timestamp,
            .jobs = {.running = tasks.size(), .max = 4},
            .tasks = std::move(tasks)};
}

} // namespace

TEST_CASE("an emerge's build is followed through its phases, merge-wait and merge") {
    EmergeWatch watch;
    auto other = building("app-misc/b-1", "compile", 95);
    CHECK(watch.observe({at(100, {building("app-misc/a-1", "setup", 99), other})}, 100).empty());
    CHECK(watch.following());
    CHECK(watch.observe({at(102, {building("app-misc/a-1", "compile", 99), other})}, 102).empty());
    auto installing = building("app-misc/a-1", "install", 99);
    installing.resources.cpu_usec = 5'000'000;
    CHECK(watch.observe({at(110, {installing, other})}, 110).empty());
    // Built at 113, as emerge froze its time, and waiting to merge.
    auto waiting = building("app-misc/a-1", "merge-wait", 99);
    waiting.merge_wait = true;
    waiting.build_elapsed = 14;
    waiting.resources = {.cpu_usec = 9'000'000, .mem_peak = 1000};
    CHECK(watch.observe({at(115, {waiting, other})}, 115).empty());
    auto merging = waiting;
    merging.kind = TaskKind::merge;
    merging.merge_wait = false;
    merging.phase = "preinst";
    CHECK(watch.observe({at(118, {merging, other})}, 118).empty());
    // A binary package and an uninstall are no builds.
    auto binary = building("app-misc/bin-1", "", 118);
    binary.binary = true;
    auto removing = building("app-misc/old-1", "", 118);
    removing.operation = "uninstall";
    const auto gone = watch.observe({at(120, {other, binary, removing})}, 120.5);
    REQUIRE(gone.size() == 1);
    CHECK(gone.at(0) == EmergeBuild{.pid = 50,
                                    .cpv = "app-misc/a-1",
                                    .root = "/",
                                    .start = 99,
                                    .phases = {{.phase = "setup", .seconds = 3},
                                               {.phase = "compile", .seconds = 8},
                                               {.phase = "install", .seconds = 3}},
                                    .built = 113,
                                    .merge_start = 118,
                                    .ended = 120.5,
                                    .resources = {.cpu_usec = 9'000'000, .mem_peak = 1000},
                                    .jobs = 4,
                                    .beside = 1,
                                    .merging = true,
                                    .emerge_running = true});

    // emerge stopped while the other built.
    const auto stopped = watch.observe({}, 125);
    REQUIRE(stopped.size() == 1);
    CHECK(stopped.at(0).cpv == "app-misc/b-1");
    // First seen 5 s after it started, past its first phase: timed from then.
    CHECK(stopped.at(0).phases == std::vector<PhaseTime>{{.phase = "compile", .seconds = 25}});
    CHECK_FALSE(stopped.at(0).built);
    CHECK_FALSE(stopped.at(0).merging);
    CHECK_FALSE(stopped.at(0).emerge_running);
    CHECK_FALSE(watch.following());
}

TEST_CASE("a build merged without waiting merges as it ends, and a failed one goes") {
    EmergeWatch watch;
    std::ignore = watch.observe({at(10, {building("app-misc/a-1", "compile", 10),
                                         building("app-misc/c-1", "configure", 10)})},
                                10);
    auto merging = building("app-misc/a-1", "", 10);
    merging.kind = TaskKind::merge;
    merging.build_elapsed = 5;
    const auto gone = watch.observe({at(16, {merging})}, 16);
    REQUIRE(gone.size() == 1);
    CHECK(gone.at(0).cpv == "app-misc/c-1");
    CHECK(gone.at(0).phases == std::vector<PhaseTime>{{.phase = "configure", .seconds = 6}});
    CHECK(gone.at(0).emerge_running);
    CHECK_FALSE(gone.at(0).merging);
    const auto merged = watch.observe({at(20, {})}, 20);
    REQUIRE(merged.size() == 1);
    CHECK(merged.at(0).built == 15);
    CHECK(merged.at(0).merge_start == 15);
    CHECK(merged.at(0).phases == std::vector<PhaseTime>{{.phase = "compile", .seconds = 5}});
}

TEST_CASE("a vdb entry says what a build was built with") {
    const egraph::test::TempDir dir;
    const auto entry = dir.path() / "app-misc" / "a-1";
    std::filesystem::create_directories(entry);
    CHECK_FALSE(egraph::read_vdb_entry(dir.path(), "app-misc/b-1"));
    egraph::test::write_text(entry / "repository", "gentoo\n");
    egraph::test::write_text(entry / "SLOT", "0/1.2\n");
    egraph::test::write_text(entry / "USE", "abi_x86_64 ssl zstd amd64\n");
    egraph::test::write_text(entry / "IUSE", "+ssl zstd x\n");
    egraph::test::write_text(entry / "CFLAGS", "-O2 -pipe\n");
    egraph::test::write_text(entry / "BUILD_TIME", "1759932131\n");
    egraph::test::write_text(entry / "SIZE", "4096\n");
    CHECK(egraph::read_vdb_entry(dir.path(), "app-misc/a-1") ==
          egraph::VdbEntry{.repo = "gentoo",
                           .slot = "0",
                           .use = {"ssl", "zstd"},
                           .environment = {{"CFLAGS", "-O2 -pipe"}},
                           .build_time = 1759932131,
                           .size = 4096});
}

TEST_CASE("an emerge build's record settles its outcome by its vdb entry") {
    const EmergeBuild merged_build{.pid = 50,
                                   .cpv = "app-misc/a-1",
                                   .root = "/",
                                   .start = 99,
                                   .phases = {{.phase = "compile", .seconds = 14}},
                                   .built = 113,
                                   .merge_start = 118,
                                   .ended = 120,
                                   .resources = {.cpu_usec = 9'000'000,
                                                 .mem_peak = 1000,
                                                 .io_read_bytes = 10,
                                                 .io_write_bytes = 20},
                                   .jobs = 4,
                                   .beside = 1,
                                   .merging = true,
                                   .emerge_running = false};
    const egraph::VdbEntry entry{.repo = "gentoo",
                                 .slot = "0",
                                 .use = {"ssl"},
                                 .environment = {{"CFLAGS", "-O2"}},
                                 .build_time = 113,
                                 .size = 5};
    const egraph::Toolchain toolchain{{"sys-devel/gcc", {{"15", "15.2.1"}, {"16", "16.2.1"}}}};
    const auto record = egraph::emerge_record(merged_build, entry, toolchain);
    REQUIRE(record);
    CHECK(record->origin == "emerge");
    CHECK(record->run == "emerge-50");
    CHECK(record->start == 99);
    CHECK(record->cpv == "app-misc/a-1");
    CHECK(record->repo == "gentoo");
    CHECK(record->slot == "0");
    CHECK(record->use == std::vector<std::string>{"ssl"});
    CHECK(record->environment == std::map<std::string, std::string>{{"CFLAGS", "-O2"}});
    CHECK(record->toolchain.at("sys-devel/gcc") == std::vector<std::string>{"15.2.1", "16.2.1"});
    CHECK(record->phases == merged_build.phases);
    CHECK(record->outcome == egraph::BuildRecord::Outcome::merged);
    CHECK(record->build_seconds == 14);
    CHECK(record->waited == 5);
    CHECK(record->merge_seconds == 2);
    CHECK(record->jobs == 4U);
    CHECK(record->beside == 1);
    CHECK(record->cpu_seconds == 9);
    CHECK(record->peak_memory == 1000U);
    CHECK(record->read_bytes == 10U);
    CHECK(record->write_bytes == 20U);
    CHECK(record->measured == egraph::Measured::cgroup);
    CHECK(record->installed_size == 5U);

    // An entry from before it started is an older install: its merge failed, while emerge ran.
    auto old = entry;
    old.build_time = 50;
    auto failed_merge = merged_build;
    failed_merge.emerge_running = true;
    const auto failed = egraph::emerge_record(failed_merge, old, toolchain);
    REQUIRE(failed);
    CHECK(failed->outcome == egraph::BuildRecord::Outcome::failed);
    CHECK(failed->failed_phase == "merge");
    CHECK_FALSE(failed->merge_seconds);
    CHECK(failed->repo.empty());
    // emerge gone with it: interrupted, unrecorded.
    CHECK_FALSE(egraph::emerge_record(merged_build, old, toolchain));
    CHECK_FALSE(egraph::emerge_record(merged_build, std::nullopt, toolchain));

    const EmergeBuild failed_build{.pid = 50,
                                   .cpv = "app-misc/c-1",
                                   .root = "/",
                                   .start = 10,
                                   .phases = {{.phase = "configure", .seconds = 6}},
                                   .ended = 16,
                                   .jobs = std::nullopt,
                                   .merging = false,
                                   .emerge_running = true};
    const auto broken = egraph::emerge_record(failed_build, std::nullopt, {});
    REQUIRE(broken);
    CHECK(broken->outcome == egraph::BuildRecord::Outcome::failed);
    CHECK(broken->failed_phase == "configure");
    CHECK(broken->build_seconds == 6);
    CHECK_FALSE(broken->jobs);
    CHECK_FALSE(broken->measured);
    CHECK_FALSE(broken->cpu_seconds);
}
