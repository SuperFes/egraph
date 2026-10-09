#include "builds.hpp"

#include "helpers.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using egraph::BuildRecord;
using egraph::BuildRecorder;
using egraph::Traced;
using egraph::WorkerEvent;
using egraph::test::TempDir;

namespace {

BuildRecord merged(std::string cpv, double start) {
    return {.origin = "exec",
            .run = "run1",
            .start = start,
            .cpv = std::move(cpv),
            .repo = "gentoo",
            .slot = "0",
            .use = {},
            .env_files = {},
            .environment = {},
            .toolchain = {},
            .phases = {},
            .build_seconds = 10,
            .merge_seconds = 2,
            .waited = 0,
            .outcome = BuildRecord::Outcome::merged,
            .failed_phase = std::nullopt,
            .status = std::nullopt,
            .log = std::nullopt,
            .error = std::nullopt,
            .jobs = std::nullopt,
            .beside = 0};
}

std::string lines(const std::vector<BuildRecord>& records) {
    std::string text;
    for (const auto& record : records) {
        text += egraph::build_record_json(record) + '\n';
    }
    return text;
}

std::vector<std::string> cpvs(const std::vector<BuildRecord>& records) {
    std::vector<std::string> found;
    for (const auto& record : records) {
        found.push_back(record.cpv);
    }
    return found;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

WorkerEvent event(WorkerEvent::Kind kind, std::string text) {
    return {.kind = kind, .text = std::move(text), .status = 0, .log = "", .values = {}};
}

std::vector<egraph::BuildStep> steps() {
    return {{.cpv = "app-misc/a-1",
             .cp = "app-misc/a",
             .version = "1",
             .repo = "gentoo",
             .slot = "0",
             .use = {"ssl", "zstd"},
             .env_files = {"notmpfs.conf"},
             .uninstall = false},
            {.cpv = "app-misc/old-1",
             .cp = "app-misc/old",
             .version = "1",
             .repo = "gentoo",
             .slot = "0",
             .use = {},
             .env_files = {},
             .uninstall = true},
            {.cpv = "sys-devel/gcc-16.2.1",
             .cp = "sys-devel/gcc",
             .version = "16.2.1",
             .repo = "gentoo",
             .slot = "16",
             .use = {},
             .env_files = {},
             .uninstall = false},
            {.cpv = "app-misc/b-2",
             .cp = "app-misc/b",
             .version = "2",
             .repo = "local",
             .slot = "2",
             .use = {},
             .env_files = {},
             .uninstall = false}};
}

egraph::Toolchain toolchain() {
    return {{"sys-devel/gcc", {{"15", "15.2.1"}}}, {"sys-libs/glibc", {{"2.2", "2.42"}}}};
}

} // namespace

TEST_CASE("the build history is kept under the root's /var/lib/egraph") {
    CHECK(egraph::builds_path("/mnt/gentoo") == "/mnt/gentoo/var/lib/egraph/builds.jsonl");
}

TEST_CASE("a build record reads back as written") {
    auto record = merged("app-misc/a-1", 1759900000.5);
    record.use = {"ssl", "zstd"};
    record.env_files = {"notmpfs.conf"};
    record.environment = {{"CFLAGS", "-O2 -pipe"}, {"MAKEOPTS", "-j8"}};
    record.toolchain = {{"sys-devel/gcc", {"15.2.1", "16.2.1"}}};
    record.phases = {{.phase = "setup", .seconds = 0.5}, {.phase = "compile", .seconds = 9.5}};
    record.waited = 1.5;
    record.jobs = 4;
    record.beside = 2;
    record.phases.back().cpu_seconds = 70;
    record.cpu_seconds = 71.5;
    record.peak_memory = 1ULL << 31U;
    record.read_bytes = 4096;
    record.write_bytes = 0;
    record.system = egraph::SystemShare{
        .cpu_busy = 0.9, .load = 7.5, .stall_cpu = 0.25, .stall_memory = 0, .stall_io = 0.01};
    record.measured = egraph::Measured::cgroup;
    record.build_stall = egraph::StallShare{.cpu = 0.5, .memory = std::nullopt, .io = 0.125};
    record.build_dir_size = 1ULL << 33U;
    record.image_size = 12288;
    record.distfiles_size = 0;
    record.installed_size = 10000;
    record.jobserver = egraph::JobserverShare{.jobs = 16, .free = 2.5};
    record.reason = egraph::BuildReason::subslot;
    record.replaced = "app-misc/a-1";
    record.rebuilt_for = "dev-libs/openssl-3.6.0";
    record.use_changed = "-gtk* qt%";
    const auto line = egraph::build_record_json(record);
    CHECK(line.find('\n') == std::string::npos);
    CHECK(egraph::parse_build_record(line) == record);

    auto failed = merged("app-misc/b-2", 1759900100);
    failed.outcome = BuildRecord::Outcome::failed;
    failed.merge_seconds.reset();
    failed.waited.reset();
    failed.failed_phase = "compile";
    failed.status = 1;
    failed.log = "/var/tmp/portage/app-misc/b-2/temp/build.log";
    CHECK(egraph::parse_build_record(egraph::build_record_json(failed)) == failed);

    auto refused = failed;
    refused.failed_phase.reset();
    refused.status.reset();
    refused.log.reset();
    refused.error = "no ebuild of app-misc/b-2::gentoo";
    CHECK(egraph::parse_build_record(egraph::build_record_json(refused)) == refused);
}

TEST_CASE("a build's reason reads back by its name, an unknown one not at all") {
    using egraph::BuildReason;
    for (const auto reason : {BuildReason::added, BuildReason::upgrade, BuildReason::downgrade,
                              BuildReason::use, BuildReason::subslot, BuildReason::reinstall}) {
        CHECK(egraph::parse_build_reason(egraph::build_reason_name(reason)) == reason);
    }
    CHECK(egraph::build_reason_name(BuildReason::added) == "new");
    CHECK(egraph::build_reason_name(BuildReason::subslot) == "subslot");
    CHECK_FALSE(egraph::parse_build_reason("sideways"));

    auto record = merged("app-misc/a-1", 1);
    record.reason = BuildReason::upgrade;
    auto line = egraph::build_record_json(record);
    CHECK(line.find(R"("reason":"upgrade")") != std::string::npos);
    const auto at = line.find("upgrade");
    line.replace(at, 7, "sideways");
    CHECK_FALSE(egraph::parse_build_record(line));
    // Records from before reasons were kept read without one.
    CHECK(
        egraph::parse_build_record(egraph::build_record_json(merged("app-misc/a-1", 1)))->reason ==
        std::nullopt);
}

TEST_CASE("a line that is not a build record, or of another version, does not parse") {
    CHECK_FALSE(egraph::parse_build_record("[]"));
    CHECK_FALSE(egraph::parse_build_record("not json"));
    CHECK_FALSE(egraph::parse_build_record(R"({"version": 1})"));
    auto line = egraph::build_record_json(merged("app-misc/a-1", 1));
    const auto version = line.find(R"("version":1)");
    REQUIRE(version != std::string::npos);
    line.replace(version, std::string_view{R"("version":1)"}.size(), R"("version":2)");
    CHECK_FALSE(egraph::parse_build_record(line));
}

TEST_CASE("the history's records are read in order, past lines that do not parse") {
    const auto text = lines({merged("app-misc/a-1", 1)}) + "garbage\n" +
                      lines({merged("app-misc/b-1", 2)}) + "\n";
    CHECK(cpvs(egraph::read_builds(text)) ==
          std::vector<std::string>{"app-misc/a-1", "app-misc/b-1"});
}

TEST_CASE("pruning keeps the last builds of each cp, in order") {
    const auto text = lines({merged("app-misc/a-1", 1), merged("app-misc/b-1", 2),
                             merged("app-misc/a-2", 3), merged("app-misc/a-3", 4)});
    CHECK(cpvs(egraph::read_builds(egraph::pruned_builds(text, 2))) ==
          std::vector<std::string>{"app-misc/b-1", "app-misc/a-2", "app-misc/a-3"});
    CHECK(egraph::pruned_builds(text, 4) == text);
    const auto appended = egraph::build_record_json(merged("app-misc/a-4", 5));
    CHECK(cpvs(egraph::read_builds(egraph::pruned_builds(text, 1, appended))) ==
          std::vector<std::string>{"app-misc/b-1", "app-misc/a-4"});
}

TEST_CASE("pruning keeps the lines it cannot read") {
    const auto text = "garbage\n" + lines({merged("app-misc/a-1", 1), merged("app-misc/a-2", 2)});
    CHECK(egraph::pruned_builds(text, 1) == "garbage\n" + lines({merged("app-misc/a-2", 2)}));
}

TEST_CASE("a record appended goes to the end of the file, the file pruned") {
    const TempDir dir;
    const auto path = dir.path() / "var/lib/egraph/builds.jsonl";
    REQUIRE(egraph::append_build(path, merged("app-misc/a-1", 1), 2));
    REQUIRE(egraph::append_build(path, merged("app-misc/b-1", 2), 2));
    REQUIRE(egraph::append_build(path, merged("app-misc/a-2", 3), 2));
    CHECK(read_file(path) ==
          lines({merged("app-misc/a-1", 1), merged("app-misc/b-1", 2), merged("app-misc/a-2", 3)}));
    REQUIRE(egraph::append_build(path, merged("app-misc/a-3", 4), 2));
    CHECK(cpvs(egraph::read_builds(read_file(path))) ==
          std::vector<std::string>{"app-misc/b-1", "app-misc/a-2", "app-misc/a-3"});
}

TEST_CASE("with build_keep 0 nothing is recorded") {
    const TempDir dir;
    const auto path = dir.path() / "builds.jsonl";
    REQUIRE(egraph::append_build(path, merged("app-misc/a-1", 1), 0));
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("a merged build's record has its phases, times, environment and toolchain") {
    using Kind = WorkerEvent::Kind;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 4};
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 100));
    auto environment = event(Kind::environment, "");
    environment.values = {{"CFLAGS", "-O2"}};
    recorder.reported(0, environment, 100);
    recorder.reported(0, event(Kind::phase, "setup"), 100);
    recorder.reported(0, event(Kind::phase, "compile"), 102);
    // Another build beside it.
    CHECK_FALSE(recorder.traced(3, Traced::build_started, 103));
    CHECK_FALSE(recorder.traced(0, Traced::built, 110));
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 115));
    recorder.reported(0, event(Kind::phase, "merge"), 115);
    recorder.reported(0, event(Kind::merged, "app-misc/a-1"), 120);
    const auto record = recorder.traced(0, Traced::merged, 120);
    REQUIRE(record);
    CHECK(record->origin == "exec");
    CHECK(record->run == "run1");
    CHECK(record->start == 100);
    CHECK(record->cpv == "app-misc/a-1");
    CHECK(record->repo == "gentoo");
    CHECK(record->slot == "0");
    CHECK(record->use == std::vector<std::string>{"ssl", "zstd"});
    CHECK(record->env_files == std::vector<std::string>{"notmpfs.conf"});
    CHECK(record->environment == std::map<std::string, std::string>{{"CFLAGS", "-O2"}});
    CHECK(record->toolchain == std::map<std::string, std::vector<std::string>>{
                                   {"sys-devel/gcc", {"15.2.1"}}, {"sys-libs/glibc", {"2.42"}}});
    CHECK(record->phases == std::vector<egraph::PhaseTime>{{.phase = "setup", .seconds = 2},
                                                           {.phase = "compile", .seconds = 8}});
    CHECK(record->build_seconds == 10);
    CHECK(record->merge_seconds == 5);
    CHECK(record->waited == 5);
    CHECK(record->outcome == BuildRecord::Outcome::merged);
    CHECK(record->jobs == 4);
    CHECK(record->beside == 1);
    CHECK(record->reason == egraph::BuildReason::added);
    CHECK_FALSE(record->replaced);
}

TEST_CASE("a build's record says why the plan merged it") {
    auto built = steps();
    built.at(0).reason = egraph::BuildReason::use;
    built.at(0).replaced = "app-misc/a-1";
    built.at(0).use_changed = "zstd*";
    built.at(3).reason = egraph::BuildReason::subslot;
    built.at(3).replaced = "app-misc/b-2";
    built.at(3).rebuilt_for = "sys-devel/gcc-16.2.1";
    BuildRecorder recorder{"exec", "run1", built, toolchain(), 4};
    const auto merge = [&](std::size_t step) {
        CHECK_FALSE(recorder.traced(step, Traced::build_started, 100));
        CHECK_FALSE(recorder.traced(step, Traced::built, 110));
        CHECK_FALSE(recorder.traced(step, Traced::merge_started, 110));
        return recorder.traced(step, Traced::merged, 111);
    };
    const auto use = merge(0);
    REQUIRE(use);
    CHECK(use->reason == egraph::BuildReason::use);
    CHECK(use->replaced == "app-misc/a-1");
    CHECK(use->use_changed == "zstd*");
    CHECK_FALSE(use->rebuilt_for);
    const auto subslot = merge(3);
    REQUIRE(subslot);
    CHECK(subslot->reason == egraph::BuildReason::subslot);
    CHECK(subslot->rebuilt_for == "sys-devel/gcc-16.2.1");
    CHECK_FALSE(subslot->use_changed);
}

namespace {

egraph::Probe probe(double cpu, std::uint64_t read, std::uint64_t written) {
    return {.worker = egraph::usage::Counters{.children_cpu = cpu,
                                              .read_bytes = read,
                                              .write_bytes = written},
            .system = std::nullopt};
}

egraph::pressure::Sample system(std::uint64_t busy, std::uint64_t total, double load,
                                std::uint64_t stalled_cpu, std::uint64_t stalled_io) {
    return {.cpu = egraph::pressure::CpuTimes{.busy = busy, .total = total},
            .load = load,
            .totals = {.cpu = stalled_cpu, .memory = 0, .io = stalled_io}};
}

} // namespace

TEST_CASE("a build is measured by its worker's counters at its start, phases and end") {
    using Kind = WorkerEvent::Kind;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 4};
    auto started = probe(5, 1000, 2000);
    started.system = system(100, 1000, 2, 0, 1'000'000);
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 100, started));
    recorder.reported(0, event(Kind::phase, "setup"), 100, probe(5, 1000, 2000));
    recorder.reported(0, event(Kind::phase, "compile"), 102, probe(5.5, 1500, 2000));
    recorder.sampled(0, 300, 4);
    recorder.sampled(0, 900, 6);
    recorder.sampled(0, 600, std::nullopt);
    auto ended = probe(45.5, 9000, 2000 + 4096);
    // Over the 10 s it built: CPU 80% busy, 2.5 s stalled on CPU and 1 s on IO.
    ended.system = system(900, 2000, 8, 2'500'000, 2'000'000);
    CHECK_FALSE(recorder.traced(0, Traced::built, 110, ended));
    // The merge is not the build's.
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 115, probe(46, 9000, 9000)));
    recorder.reported(0, event(Kind::phase, "merge"), 115, probe(46, 9000, 9000));
    recorder.sampled(0, 5000, 20);
    const auto record = recorder.traced(0, Traced::merged, 120, probe(50, 20000, 20000));
    REQUIRE(record);
    CHECK(record->phases ==
          std::vector<egraph::PhaseTime>{{.phase = "setup", .seconds = 2, .cpu_seconds = 0.5},
                                         {.phase = "compile", .seconds = 8, .cpu_seconds = 40}});
    CHECK(record->cpu_seconds == 40.5);
    CHECK(record->peak_memory == 900U);
    CHECK(record->read_bytes == 8000U);
    CHECK(record->write_bytes == 4096U);
    REQUIRE(record->system);
    CHECK(record->system->cpu_busy == 0.8);
    // The loads at its start, sampled and at its end.
    CHECK(record->system->load == 5);
    CHECK(record->system->stall_cpu == 0.25);
    CHECK(record->system->stall_memory == 0);
    CHECK(record->system->stall_io == 0.1);
    CHECK(record->measured == egraph::Measured::proc);
    CHECK_FALSE(record->build_stall);
}

TEST_CASE("a build in its own cgroup is measured by its leaf") {
    using Kind = WorkerEvent::Kind;
    using egraph::cgroup::Counters;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 4};
    const auto in_leaf = [](double worker_cpu, std::optional<Counters> leaf) {
        auto probed = probe(worker_cpu, 1000, 2000);
        probed.cgroup = std::move(leaf);
        return probed;
    };
    // The leaf is made by the first phase's processes.
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 100, in_leaf(5, std::nullopt)));
    recorder.reported(0, event(Kind::phase, "setup"), 100, in_leaf(5, std::nullopt));
    recorder.reported(0, event(Kind::phase, "compile"), 102,
                      in_leaf(5.5, Counters{.cpu_usec = 750'000}));
    recorder.sampled(0, 300, std::nullopt);
    // Over the 10 s it built, its own tasks stalled 2.5 s on CPU; the kernel kept no IO
    // pressure.
    const Counters end{.cpu_usec = 40'750'000,
                       .memory_peak = 4096,
                       .read_bytes = 100,
                       .write_bytes = 200,
                       .stalls = {.cpu = 2'500'000, .memory = 0, .io = std::nullopt}};
    CHECK_FALSE(recorder.traced(0, Traced::built, 110, in_leaf(45.5, end)));
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 115, probe(46, 9000, 9000)));
    const auto record = recorder.traced(0, Traced::merged, 120, probe(50, 20000, 20000));
    REQUIRE(record);
    CHECK(record->phases ==
          std::vector<egraph::PhaseTime>{{.phase = "setup", .seconds = 2, .cpu_seconds = 0.75},
                                         {.phase = "compile", .seconds = 8, .cpu_seconds = 40}});
    CHECK(record->cpu_seconds == 40.75);
    CHECK(record->peak_memory == 4096U);
    CHECK(record->read_bytes == 100U);
    CHECK(record->write_bytes == 200U);
    CHECK(record->measured == egraph::Measured::cgroup);
    CHECK(record->build_stall == egraph::StallShare{.cpu = 0.25, .memory = 0, .io = std::nullopt});

    // A leaf without the memory and io controllers: those from /proc.
    CHECK_FALSE(recorder.traced(3, Traced::build_started, 200, in_leaf(1, std::nullopt)));
    recorder.reported(3, event(Kind::phase, "compile"), 200, in_leaf(1, std::nullopt));
    recorder.sampled(3, 700, std::nullopt);
    auto bare = in_leaf(9, Counters{.cpu_usec = 2'000'000});
    bare.worker->read_bytes = 1500;
    recorder.reported(
        3, {.kind = Kind::failed, .text = "compile", .status = 1, .log = "/l", .values = {}}, 204,
        bare);
    const auto failed = recorder.traced(3, Traced::build_failed, 204, bare);
    REQUIRE(failed);
    CHECK(failed->phases.at(0).cpu_seconds == 2);
    CHECK(failed->cpu_seconds == 2);
    CHECK(failed->peak_memory == 700U);
    CHECK(failed->read_bytes == 500U);
    CHECK(failed->write_bytes == 0U);
    CHECK(failed->measured == egraph::Measured::cgroup);
    CHECK_FALSE(failed->build_stall);
}

TEST_CASE("a build is measured as far as its probes go") {
    using Kind = WorkerEvent::Kind;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 4};
    // Unmeasured: nothing.
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 100));
    recorder.reported(0, event(Kind::phase, "compile"), 100);
    CHECK_FALSE(recorder.traced(0, Traced::built, 110));
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 110));
    auto record = recorder.traced(0, Traced::merged, 111);
    REQUIRE(record);
    CHECK_FALSE(record->phases.at(0).cpu_seconds);
    CHECK_FALSE(record->cpu_seconds);
    CHECK_FALSE(record->peak_memory);
    CHECK_FALSE(record->read_bytes);
    CHECK_FALSE(record->system);
    CHECK_FALSE(record->measured);

    // A worker whose IO is not accounted, and a failure with its processes' time.
    egraph::Probe start{.worker = egraph::usage::Counters{.children_cpu = 1}, .system = {}};
    CHECK_FALSE(recorder.traced(3, Traced::build_started, 200, start));
    recorder.reported(3, event(Kind::phase, "compile"), 200, start);
    egraph::Probe end{.worker = egraph::usage::Counters{.children_cpu = 4}, .system = {}};
    recorder.reported(
        3, {.kind = Kind::failed, .text = "compile", .status = 1, .log = "/l", .values = {}}, 203,
        end);
    record = recorder.traced(3, Traced::build_failed, 203, end);
    REQUIRE(record);
    CHECK(record->phases.at(0).cpu_seconds == 3);
    CHECK(record->cpu_seconds == 3);
    CHECK_FALSE(record->read_bytes);
    CHECK_FALSE(record->system);

    // A sample for a step not building is not its.
    recorder.sampled(2, 100, 1);
    CHECK_FALSE(recorder.traced(2, Traced::build_started, 300));
    CHECK_FALSE(recorder.traced(2, Traced::built, 301));
    CHECK_FALSE(recorder.traced(2, Traced::merge_started, 301));
    record = recorder.traced(2, Traced::merged, 302);
    REQUIRE(record);
    CHECK_FALSE(record->peak_memory);
}

TEST_CASE("a failed build's record names the phase, its status and its log") {
    using Kind = WorkerEvent::Kind;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), std::nullopt};
    CHECK_FALSE(recorder.traced(3, Traced::build_started, 10));
    recorder.reported(3, event(Kind::phase, "setup"), 10);
    recorder.reported(3, event(Kind::phase, "compile"), 11);
    recorder.reported(
        3, {.kind = Kind::failed, .text = "compile", .status = 1, .log = "/l", .values = {}}, 15);
    const auto record = recorder.traced(3, Traced::build_failed, 15);
    REQUIRE(record);
    CHECK(record->outcome == BuildRecord::Outcome::failed);
    CHECK(record->phases == std::vector<egraph::PhaseTime>{{.phase = "setup", .seconds = 1},
                                                           {.phase = "compile", .seconds = 4}});
    CHECK(record->build_seconds == 5);
    CHECK_FALSE(record->merge_seconds);
    CHECK(record->failed_phase == "compile");
    CHECK(record->status == 1);
    CHECK(record->log == "/l");
    CHECK_FALSE(record->jobs);
    CHECK(record->beside == 0);
}

TEST_CASE("a failed merge's record names the merge, a worker's refusal its error") {
    using Kind = WorkerEvent::Kind;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 1};
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 0));
    CHECK_FALSE(recorder.traced(0, Traced::built, 5));
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 5));
    recorder.reported(0, event(Kind::phase, "merge"), 5);
    recorder.reported(
        0, {.kind = Kind::failed, .text = "merge", .status = 1, .log = "/m", .values = {}}, 7);
    const auto merge = recorder.traced(0, Traced::merge_failed, 7);
    REQUIRE(merge);
    CHECK(merge->failed_phase == "merge");
    CHECK(merge->build_seconds == 5);

    CHECK_FALSE(recorder.traced(3, Traced::build_started, 10));
    recorder.reported(3, event(Kind::error, "no ebuild of app-misc/b-2::local"), 10);
    const auto refused = recorder.traced(3, Traced::build_failed, 10);
    REQUIRE(refused);
    CHECK(refused->error == "no ebuild of app-misc/b-2::local");
    CHECK_FALSE(refused->failed_phase);
}

TEST_CASE("an uninstall or a skip records nothing") {
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 1};
    CHECK_FALSE(recorder.traced(1, Traced::merge_started, 0));
    CHECK_FALSE(recorder.traced(1, Traced::merged, 1));
    CHECK_FALSE(recorder.traced(3, Traced::skipped, 2));
}

TEST_CASE("a toolchain merged in the run is the toolchain of the builds after it") {
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 1};
    // a starts before gcc 16 merges, b after.
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 0));
    CHECK_FALSE(recorder.traced(2, Traced::build_started, 1));
    CHECK_FALSE(recorder.traced(2, Traced::built, 2));
    CHECK_FALSE(recorder.traced(2, Traced::merge_started, 2));
    const auto gcc = recorder.traced(2, Traced::merged, 3);
    REQUIRE(gcc);
    CHECK(gcc->toolchain.at("sys-devel/gcc") == std::vector<std::string>{"15.2.1"});
    CHECK_FALSE(recorder.traced(3, Traced::build_started, 4));
    CHECK_FALSE(recorder.traced(0, Traced::built, 5));
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 5));
    const auto a = recorder.traced(0, Traced::merged, 6);
    REQUIRE(a);
    CHECK(a->toolchain.at("sys-devel/gcc") == std::vector<std::string>{"15.2.1"});
    CHECK_FALSE(recorder.traced(3, Traced::built, 7));
    CHECK_FALSE(recorder.traced(3, Traced::merge_started, 7));
    const auto b = recorder.traced(3, Traced::merged, 8);
    REQUIRE(b);
    CHECK(b->toolchain.at("sys-devel/gcc") == std::vector<std::string>{"15.2.1", "16.2.1"});
}

TEST_CASE("a worker reports the environment a build starts in, which ends nothing") {
    const auto parsed =
        egraph::parse_event(R"({"environment": {"CFLAGS": "-O2", "MAKEOPTS": "-j4"}})");
    REQUIRE(parsed);
    CHECK(parsed->kind == WorkerEvent::Kind::environment);
    CHECK(parsed->values ==
          std::map<std::string, std::string>{{"CFLAGS", "-O2"}, {"MAKEOPTS", "-j4"}});
    CHECK_FALSE(egraph::final_event(*parsed));
    CHECK_FALSE(egraph::parse_event(R"({"environment": {"CFLAGS": 2}})"));
    CHECK_FALSE(egraph::parse_event(R"({"environment": "CFLAGS"})"));
}

TEST_CASE("a worker reports a build's sizes, which ends nothing") {
    const auto parsed =
        egraph::parse_event(R"({"sizes": {"build": 122880, "image": 12288, "distfiles": 0}})");
    REQUIRE(parsed);
    CHECK(parsed->kind == WorkerEvent::Kind::sizes);
    CHECK(parsed->sizes == std::map<std::string, std::uint64_t>{
                               {"build", 122880}, {"distfiles", 0}, {"image", 12288}});
    CHECK_FALSE(egraph::final_event(*parsed));
    CHECK_FALSE(egraph::parse_event(R"({"sizes": {"build": -1}})"));
    CHECK_FALSE(egraph::parse_event(R"({"sizes": {"build": 1.5}})"));
    CHECK_FALSE(egraph::parse_event(R"({"sizes": 3})"));
}

TEST_CASE("a build's record holds its sizes and the jobserver's tokens while it built") {
    using Kind = WorkerEvent::Kind;
    BuildRecorder recorder{"exec", "run1", steps(), toolchain(), 4};
    CHECK_FALSE(recorder.traced(0, Traced::build_started, 100));
    recorder.reported(0, event(Kind::phase, "compile"), 100);
    recorder.sampled(0, 10, std::nullopt, egraph::JobserverShare{.jobs = 16, .free = 0});
    recorder.sampled(0, 10, std::nullopt);
    recorder.sampled(0, 10, std::nullopt, egraph::JobserverShare{.jobs = 8, .free = 3});
    recorder.reported(0,
                      {.kind = Kind::sizes,
                       .text = "",
                       .status = 0,
                       .log = "",
                       .values = {},
                       .sizes = {{"build", 40960}, {"image", 4096}, {"distfiles", 77}}},
                      104);
    CHECK_FALSE(recorder.traced(0, Traced::built, 105));
    // The merge's samples are not the build's.
    recorder.sampled(0, 10, std::nullopt, egraph::JobserverShare{.jobs = 1, .free = 1});
    CHECK_FALSE(recorder.traced(0, Traced::merge_started, 105));
    const auto record = recorder.traced(0, Traced::merged, 106);
    REQUIRE(record);
    CHECK(record->build_dir_size == 40960U);
    CHECK(record->image_size == 4096U);
    CHECK(record->distfiles_size == 77U);
    // Read from the vdb once merged, by exec.
    CHECK_FALSE(record->installed_size);
    CHECK(record->jobserver == egraph::JobserverShare{.jobs = 12, .free = 1.5});
    // The sizes ended no phase.
    CHECK(record->phases.size() == 1);
    CHECK(record->phases.at(0).seconds == 5);

    CHECK_FALSE(recorder.traced(3, Traced::build_started, 200));
    CHECK_FALSE(recorder.traced(3, Traced::built, 201));
    CHECK_FALSE(recorder.traced(3, Traced::merge_started, 201));
    const auto unsized = recorder.traced(3, Traced::merged, 202);
    REQUIRE(unsized);
    CHECK_FALSE(unsized->build_dir_size);
    CHECK_FALSE(unsized->jobserver);
}

TEST_CASE("a merged package's installed size is its vdb entry's") {
    const egraph::test::TempDir dir;
    std::filesystem::create_directories(dir.path() / "app-misc" / "a-1");
    egraph::test::write_text(dir.path() / "app-misc" / "a-1" / "SIZE", "123456\n");
    CHECK(egraph::installed_size(dir.path(), "app-misc/a-1") == 123456U);
    CHECK_FALSE(egraph::installed_size(dir.path(), "app-misc/b-1"));
    egraph::test::write_text(dir.path() / "app-misc" / "a-1" / "SIZE", "big\n");
    CHECK_FALSE(egraph::installed_size(dir.path(), "app-misc/a-1"));
}

TEST_CASE("a step's record names the candidate it builds with its flags, or what it uninstalls") {
    const auto system = egraph::test::make_system(
        {{.cpv = "app-misc/old-1"}, {.cpv = "sys-devel/gcc-15.2.1", .slot = "15"}},
        {{.cpv = "app-misc/a-1",
          .slot = "2",
          .repo = "local",
          .iuse = "zstd ssl x",
          .use = "zstd ssl"}});
    egraph::Plan plan;
    plan.merges.push_back({});
    plan.merges.back().candidate = 0;
    plan.uninstalls.push_back({.package = 0, .why = std::nullopt, .after = {}});
    const std::vector<egraph::Step> steps{
        egraph::MergeStep{.merge = 0, .blockers = {}, .world = {}},
        egraph::UninstallStep{.uninstall = 0, .clean_world = false}};
    const auto found = egraph::build_steps(system.store, system.evaluated, plan, steps);
    REQUIRE(found.size() == 2);
    CHECK(found.at(0).cpv == "app-misc/a-1");
    CHECK(found.at(0).cp == "app-misc/a");
    CHECK(found.at(0).version == "1");
    CHECK(found.at(0).repo == "local");
    CHECK(found.at(0).slot == "2");
    CHECK(found.at(0).use == std::vector<std::string>{"ssl", "zstd"});
    CHECK_FALSE(found.at(0).uninstall);
    CHECK(found.at(0).reason == egraph::BuildReason::added);
    CHECK_FALSE(found.at(0).replaced);
    CHECK(found.at(1).cpv == "app-misc/old-1");
    CHECK(found.at(1).uninstall);

    CHECK(egraph::installed_toolchain(system.store) ==
          egraph::Toolchain{{"sys-devel/gcc", {{"15", "15.2.1"}}}});
}

TEST_CASE("a step says why the plan merges it, and what it replaces") {
    using egraph::BuildReason;
    using egraph::UpdateKind;
    const auto system = egraph::test::make_system(
        {{.cpv = "app-misc/a-1"}, {.cpv = "app-misc/b-2"}, {.cpv = "dev-libs/c-1"}},
        {{.cpv = "app-misc/a-2"},
         {.cpv = "app-misc/b-1"},
         {.cpv = "dev-libs/c-1"},
         {.cpv = "dev-libs/c-1"},
         {.cpv = "dev-libs/c-1"},
         {.cpv = "dev-libs/d-1"}});
    egraph::Plan plan;
    const auto merge = [&](std::uint32_t candidate, std::optional<std::uint32_t> replaces,
                           UpdateKind kind, std::string flags = {}) {
        plan.merges.push_back({});
        plan.merges.back().candidate = candidate;
        plan.merges.back().replaces = replaces;
        plan.merges.back().kind = kind;
        plan.merges.back().flags = std::move(flags);
    };
    merge(0, 0, UpdateKind::upgrade);
    merge(1, 1, UpdateKind::downgrade);
    merge(2, 2, UpdateKind::rebuild, "-gtk* qt%");
    merge(3, 2, UpdateKind::rebuild);
    plan.merges.back().rebuilt_for =
        egraph::Reason{.member = {.candidate = true, .index = 5}, .atom = "dev-libs/d:="};
    merge(4, 2, UpdateKind::rebuild);
    merge(5, std::nullopt, UpdateKind::upgrade);
    std::vector<egraph::Step> steps;
    for (std::uint32_t i = 0; i < plan.merges.size(); ++i) {
        steps.emplace_back(egraph::MergeStep{.merge = i, .blockers = {}, .world = {}});
    }
    const auto found = egraph::build_steps(system.store, system.evaluated, plan, steps);
    REQUIRE(found.size() == 6);
    CHECK(found.at(0).reason == BuildReason::upgrade);
    CHECK(found.at(0).replaced == "app-misc/a-1");
    CHECK(found.at(1).reason == BuildReason::downgrade);
    CHECK(found.at(1).replaced == "app-misc/b-2");
    CHECK(found.at(2).reason == BuildReason::use);
    CHECK(found.at(2).use_changed == "-gtk* qt%");
    CHECK(found.at(3).reason == BuildReason::subslot);
    CHECK(found.at(3).rebuilt_for == "dev-libs/d-1");
    CHECK_FALSE(found.at(3).use_changed);
    CHECK(found.at(4).reason == BuildReason::reinstall);
    CHECK(found.at(4).replaced == "dev-libs/c-1");
    CHECK(found.at(5).reason == BuildReason::added);
    CHECK_FALSE(found.at(5).replaced);
}

TEST_CASE("the builds shown are those the atoms or cps given match") {
    auto other = merged("app-misc/b-2", 2);
    other.slot = "2";
    other.repo = "local";
    const std::vector<BuildRecord> records{merged("app-misc/a-1", 1), other,
                                           merged("app-misc/a-2", 3)};
    const auto pick = [&](std::vector<std::string> packages) {
        const auto found = egraph::matching_builds(records, packages);
        REQUIRE(found);
        return cpvs(*found);
    };
    CHECK(pick({}).size() == 3);
    CHECK(pick({"app-misc/a"}) == std::vector<std::string>{"app-misc/a-1", "app-misc/a-2"});
    CHECK(pick({">=app-misc/a-2", "app-misc/b:2"}) ==
          std::vector<std::string>{"app-misc/b-2", "app-misc/a-2"});
    CHECK(pick({"app-misc/b::gentoo"}).empty());
    CHECK_FALSE(egraph::matching_builds(records, std::vector<std::string>{"not an atom"}));
}

TEST_CASE("for scripts, a build's fields are separated by tabs") {
    auto record = merged("app-misc/a-1", 1759932131.6);
    record.jobs = 4;
    record.beside = 2;
    CHECK(egraph::build_fields(record) ==
          "1759932132\tapp-misc/a-1\tgentoo\tmerged\t10.0\t2.0\t\t4\t2\t\t\t");
    record.cpu_seconds = 61.25;
    record.peak_memory = 1048576;
    record.reason = egraph::BuildReason::upgrade;
    CHECK(egraph::build_fields(record) == "1759932132\tapp-misc/a-1\tgentoo\tmerged\t10.0\t2."
                                          "0\t\t4\t2\t61.2\t1048576\tupgrade");
    record.reason.reset();
    record.cpu_seconds.reset();
    record.peak_memory.reset();
    record.outcome = BuildRecord::Outcome::failed;
    record.merge_seconds.reset();
    record.failed_phase = "compile";
    record.jobs.reset();
    CHECK(egraph::build_fields(record) ==
          "1759932132\tapp-misc/a-1\tgentoo\tfailed\t10.0\t\tcompile\t\t2\t\t\t");
}

TEST_CASE("as CSV, a header and a row per build, quoted where it must be") {
    CHECK(egraph::builds_csv_header().starts_with("start,origin,run,cpv,"));
    auto record = merged("app-misc/a-1", 1759932131);
    record.use = {"ssl", "zstd"};
    record.error = R"(said "no", twice)";
    CHECK(egraph::build_csv_row(record) ==
          R"(2025-10-08T14:02:11Z,exec,run1,app-misc/a-1,gentoo,0,merged,10.0,2.0,0.0,,,)"
          R"("said ""no"", twice",,0,,,,,,,,,,,,,,,,,,,,ssl zstd,,,,,,)");
    record.error.reset();
    record.cpu_seconds = 61.25;
    record.peak_memory = 1048576;
    record.read_bytes = 4096;
    record.write_bytes = 0;
    record.system = egraph::SystemShare{
        .cpu_busy = 0.5, .load = 3.25, .stall_cpu = 0.125, .stall_memory = 0, .stall_io = 0.5};
    record.measured = egraph::Measured::cgroup;
    record.build_stall = egraph::StallShare{.cpu = 0.25, .memory = 0, .io = std::nullopt};
    record.build_dir_size = 40960;
    record.image_size = 4096;
    record.installed_size = 1000;
    record.jobserver = egraph::JobserverShare{.jobs = 16, .free = 0.25};
    CHECK(egraph::build_csv_row(record) ==
          R"(2025-10-08T14:02:11Z,exec,run1,app-misc/a-1,gentoo,0,merged,10.0,2.0,0.0,,,,,0,)"
          R"(61.2,1048576,4096,0,cgroup,0.500,3.25,0.125,0.000,0.500,0.250,0.000,,)"
          R"(40960,4096,,1000,16.0,0.25,ssl zstd,,,,,,)");
    record.reason = egraph::BuildReason::use;
    record.replaced = "app-misc/a-1";
    record.use_changed = "-gtk* qt%";
    CHECK(egraph::build_csv_row(record).ends_with(",ssl zstd,,,use,app-misc/a-1,,-gtk* qt%"));
    const auto header = egraph::builds_csv_header();
    CHECK(header.ends_with(",log,reason,replaced,rebuilt_for,use_changed"));
    CHECK(std::ranges::count(header, ',') == 40);
}
