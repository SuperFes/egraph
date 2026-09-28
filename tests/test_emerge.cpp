#include "emerge.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace {

// As portage's build_snapshot() writes it: one package compiling with cgroup counters, one
// built and waiting to merge, and one binary package whose first phase has not started.
constexpr std::string_view snapshot_json = R"({
  "type": "snapshot", "schema": 1, "emerge_pid": 4321, "timestamp": 1790000000.5,
  "jobs": {"running": 3, "max": 4, "completed": 12, "total": 40, "failed": 1,
           "merge_wait": 1, "merges_pending": 0},
  "tasks": [
    {"cpv": "dev-libs/foo-1.2", "category": "dev-libs", "pf": "foo-1.2", "root": "/",
     "operation": "merge", "binary": false, "kind": "build", "phase": "compile",
     "merge_wait": false, "pid": 5000, "start_time": 1789999900.0, "elapsed": 100.5,
     "build_elapsed": 100.5,
     "resources": {"cpu_usec": 402000000, "mem_current": 1048576, "mem_peak": 2097152,
                   "io_read_bytes": 0, "io_write_bytes": 4096}},
    {"cpv": "sys-apps/bar-3", "kind": "merge", "phase": "merge-wait", "merge_wait": true,
     "binary": false, "pid": null, "elapsed": 60, "build_elapsed": 55.25},
    {"cpv": "app-misc/baz-1", "kind": "build", "phase": null, "binary": true, "pid": 5002,
     "elapsed": null, "build_elapsed": null, "resources": {"mem_peak": "lots"}}
  ]
})";

std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out{text};
    const auto at = out.find(from);
    REQUIRE(at != std::string::npos);
    out.replace(at, from.size(), to);
    return out;
}

} // namespace

TEST_CASE("a snapshot's jobs and tasks are read") {
    const auto snapshot = egraph::emerge::parse_snapshot(snapshot_json);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->pid == 4321);
    CHECK(snapshot->timestamp == 1790000000.5);
    CHECK(snapshot->jobs.running == 3);
    CHECK(snapshot->jobs.max == 4);
    CHECK(snapshot->jobs.completed == 12);
    CHECK(snapshot->jobs.total == 40);
    CHECK(snapshot->jobs.failed == 1);
    CHECK(snapshot->jobs.merge_wait == 1);
    REQUIRE(snapshot->tasks.size() == 3);

    const auto& compiling = snapshot->tasks.at(0);
    CHECK(compiling.cpv == "dev-libs/foo-1.2");
    CHECK(compiling.kind == egraph::emerge::TaskKind::build);
    CHECK(compiling.phase == "compile");
    CHECK(compiling.pid == 5000);
    CHECK(compiling.elapsed == 100.5);
    CHECK(compiling.resources.cpu_usec == 402000000);
    CHECK(compiling.resources.mem_peak == 2097152);
    CHECK(compiling.resources.io_read_bytes == 0);

    const auto& waiting = snapshot->tasks.at(1);
    CHECK(waiting.kind == egraph::emerge::TaskKind::merge);
    CHECK(waiting.merge_wait);
    CHECK_FALSE(waiting.pid.has_value());
    CHECK(waiting.elapsed == 60.0);
    CHECK(waiting.build_elapsed == 55.25);
    CHECK_FALSE(waiting.resources.cpu_usec.has_value());

    // null and mistyped fields keep their defaults.
    const auto& binary = snapshot->tasks.at(2);
    CHECK(binary.binary);
    CHECK(binary.phase.empty());
    CHECK_FALSE(binary.elapsed.has_value());
    CHECK_FALSE(binary.resources.mem_peak.has_value());
}

TEST_CASE("--jobs without a limit has no maximum") {
    const auto snapshot =
        egraph::emerge::parse_snapshot(replaced(snapshot_json, R"("max": 4)", R"("max": true)"));
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->jobs.max.has_value());
}

TEST_CASE("only schema 1 snapshots are read") {
    CHECK_FALSE(egraph::emerge::parse_snapshot("").has_value());
    CHECK_FALSE(egraph::emerge::parse_snapshot("{\"type\": ").has_value());
    CHECK_FALSE(egraph::emerge::parse_snapshot("[]").has_value());
    CHECK_FALSE(egraph::emerge::parse_snapshot(
                    replaced(snapshot_json, R"("type": "snapshot")", R"("type": "event")"))
                    .has_value());
    CHECK_FALSE(
        egraph::emerge::parse_snapshot(replaced(snapshot_json, R"("schema": 1)", R"("schema": 2)"))
            .has_value());
    CHECK_FALSE(egraph::emerge::parse_snapshot(
                    replaced(snapshot_json, R"("emerge_pid": 4321)", R"("emerge_pid": "x")"))
                    .has_value());
}

TEST_CASE("status files are under the prefix's run directory") {
    CHECK(egraph::emerge::status_dir("") == "/run/portage");
    CHECK(egraph::emerge::status_dir("/p") == "/p/run/portage");
}

TEST_CASE("only live emerges' snapshots are read, by pid") {
    const egraph::test::TempDir dir;
    const auto run = dir.path() / "run";
    const auto proc = dir.path() / "proc";
    std::filesystem::create_directories(run);
    for (const auto* pid : {"4321", "77"}) {
        std::filesystem::create_directories(proc / pid);
    }
    egraph::test::write_text(run / "emerge-4321.json", snapshot_json);
    egraph::test::write_text(
        run / "emerge-77.json",
        replaced(snapshot_json, R"("emerge_pid": 4321)", R"("emerge_pid": 77)"));
    // Its emerge died, and the pid inside does not match the name.
    egraph::test::write_text(
        run / "emerge-900.json",
        replaced(snapshot_json, R"("emerge_pid": 4321)", R"("emerge_pid": 900)"));
    egraph::test::write_text(run / "emerge-78.json", snapshot_json);
    std::filesystem::create_directories(proc / "78");
    // Neither a status file nor readable.
    egraph::test::write_text(run / "emerge-79.sock", "");
    egraph::test::write_text(run / "emerge-80.json", "{");
    std::filesystem::create_directories(proc / "80");
    egraph::test::write_text(run / "other-81.json", snapshot_json);

    const auto found = egraph::emerge::read_snapshots(run, proc);
    REQUIRE(found.size() == 2);
    CHECK(found.at(0).pid == 77);
    CHECK(found.at(1).pid == 4321);

    CHECK(egraph::emerge::read_snapshots(dir.path() / "missing", proc).empty());
}

namespace {

std::vector<egraph::emerge::Pending> pending_list(std::initializer_list<std::string_view> cpvs) {
    std::vector<egraph::emerge::Pending> list;
    for (const auto cpv : cpvs) {
        list.push_back({.kind = "ebuild", .root = "/", .cpv = std::string{cpv}});
    }
    return list;
}

} // namespace

TEST_CASE("the merge list is mtimedb's resume mergelist") {
    CHECK(egraph::emerge::mtimedb_path("") == "/var/cache/edb/mtimedb");
    CHECK(egraph::emerge::mtimedb_path("/p") == "/p/var/cache/edb/mtimedb");
    const auto list = egraph::emerge::parse_mergelist(R"({
      "info": {}, "resume": {"favorites": ["games-util/lutris"], "myopts": {"--jobs": 4},
        "mergelist": [["ebuild", "/", "net-libs/webkit-gtk-2.54.0-r410", "merge"],
                      ["binary", "/", "games-util/lutris-0.5.22-r1", "merge"],
                      ["ebuild", "/"], "junk"]},
      "resume_backup": {"mergelist": [["ebuild", "/", "x/old-1", "merge"]]}})");
    REQUIRE(list.size() == 2);
    CHECK(list.at(0).cpv == "net-libs/webkit-gtk-2.54.0-r410");
    CHECK(list.at(0).kind == "ebuild");
    CHECK(list.at(1).kind == "binary");
    CHECK(list.at(1).root == "/");
    // No emerge running, or an interrupted one's list kept for --resume only in resume_backup.
    CHECK(egraph::emerge::parse_mergelist(R"({"resume_backup": {"mergelist": []}})").empty());
    CHECK(egraph::emerge::parse_mergelist("{").empty());
}

TEST_CASE("waits are read as the builder writes them") {
    const auto waits = egraph::emerge::parse_waits(
        R"({"games-util/lutris-0.5.22-r1": ["net-libs/webkit-gtk-2.54.0-r410"], "x/y-1": []})");
    REQUIRE(waits.has_value());
    CHECK(waits->at("games-util/lutris-0.5.22-r1") ==
          std::vector<std::string>{"net-libs/webkit-gtk-2.54.0-r410"});
    CHECK(waits->at("x/y-1").empty());
    CHECK_FALSE(egraph::emerge::parse_waits("[]").has_value());
    CHECK_FALSE(egraph::emerge::parse_waits(R"({"x/y-1": [1]})").has_value());
    CHECK_FALSE(egraph::emerge::parse_waits("{").has_value());
}

TEST_CASE("the merge list hangs each package under what it waits for last") {
    // lib and tool first; app waits for lib, plugin for app and lib, doc for nothing earlier;
    // lib's run-time loop back to plugin is left to emerge's order.
    const auto list = pending_list({"dev-libs/lib-1", "dev-util/tool-1", "app-misc/app-1",
                                    "app-misc/plugin-1", "app-doc/doc-1"});
    const egraph::emerge::Waits waits{{"dev-libs/lib-1", {"app-misc/plugin-1"}},
                                      {"app-misc/app-1", {"dev-libs/lib-1", "dev-util/tool-1"}},
                                      {"app-misc/plugin-1", {"app-misc/app-1", "dev-libs/lib-1"}},
                                      {"app-doc/doc-1", {"x/merged-already-1"}}};
    const auto tree = egraph::emerge::hierarchy(list, waits);
    REQUIRE(tree.size() == 5);
    std::vector<std::pair<std::string, std::size_t>> shape;
    for (const auto& branch : tree) {
        shape.emplace_back(branch.cpv, branch.depth);
    }
    CHECK(shape == std::vector<std::pair<std::string, std::size_t>>{{"dev-libs/lib-1", 1},
                                                                    {"dev-util/tool-1", 1},
                                                                    {"app-misc/app-1", 2},
                                                                    {"app-misc/plugin-1", 3},
                                                                    {"app-doc/doc-1", 1}});
    // Counts every pending wait, the loop included; not what already merged.
    CHECK(tree.at(0).waiting_for == 1);
    CHECK(tree.at(2).waiting_for == 2);
    CHECK(tree.at(3).waiting_for == 2);
    CHECK(tree.at(4).waiting_for == 0);
    // lib and tool have siblings after them at the top; plugin is app's only child.
    CHECK_FALSE(tree.at(0).last);
    CHECK_FALSE(tree.at(1).last);
    CHECK(tree.at(3).last);
    CHECK(tree.at(4).last);
    // The top level's line runs on past app and plugin, down to doc.
    CHECK(tree.at(3).rails == std::vector<bool>{true, false});

    // Without waits yet, a flat list in merge order.
    const auto flat = egraph::emerge::hierarchy(list, {});
    CHECK(std::ranges::all_of(flat, [](const auto& branch) { return branch.depth == 1; }));
}
