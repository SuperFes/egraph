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
    CHECK(found.at(1).cpv == "app-misc/old-1");
    CHECK(found.at(1).uninstall);

    CHECK(egraph::installed_toolchain(system.store) ==
          egraph::Toolchain{{"sys-devel/gcc", {{"15", "15.2.1"}}}});
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

TEST_CASE("a build reads as when, what, how it ended and how long it took") {
    const auto* utc = std::chrono::locate_zone("UTC");
    auto record = merged("app-misc/a-1", 1759932131);
    record.build_seconds = 123;
    record.merge_seconds = 4;
    CHECK(egraph::build_line(record, *utc) ==
          "2025-10-08 14:02:11  app-misc/a-1::gentoo  merged in 2 min 3 s, merge 4.0 s");
    record.outcome = BuildRecord::Outcome::failed;
    record.merge_seconds.reset();
    record.failed_phase = "compile";
    CHECK(egraph::build_line(record, *utc) ==
          "2025-10-08 14:02:11  app-misc/a-1::gentoo  failed in compile after 2 min 3 s");
    record.failed_phase.reset();
    record.error = "no ebuild";
    CHECK(egraph::build_line(record, *utc) ==
          "2025-10-08 14:02:11  app-misc/a-1::gentoo  failed after 2 min 3 s: no ebuild");
}

TEST_CASE("for scripts, a build's fields are separated by tabs") {
    auto record = merged("app-misc/a-1", 1759932131.6);
    record.jobs = 4;
    record.beside = 2;
    CHECK(egraph::build_fields(record) ==
          "1759932132\tapp-misc/a-1\tgentoo\tmerged\t10.0\t2.0\t\t4\t2");
    record.outcome = BuildRecord::Outcome::failed;
    record.merge_seconds.reset();
    record.failed_phase = "compile";
    record.jobs.reset();
    CHECK(egraph::build_fields(record) ==
          "1759932132\tapp-misc/a-1\tgentoo\tfailed\t10.0\t\tcompile\t\t2");
}

TEST_CASE("as CSV, a header and a row per build, quoted where it must be") {
    CHECK(egraph::builds_csv_header().starts_with("start,origin,run,cpv,"));
    auto record = merged("app-misc/a-1", 1759932131);
    record.use = {"ssl", "zstd"};
    record.error = R"(said "no", twice)";
    CHECK(egraph::build_csv_row(record) ==
          R"(2025-10-08T14:02:11Z,exec,run1,app-misc/a-1,gentoo,0,merged,10.0,2.0,0.0,,,)"
          R"("said ""no"", twice",,0,ssl zstd,,)");
}
