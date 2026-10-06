#include "run_log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

using egraph::Traced;
using egraph::WorkerEvent;
using egraph::log::Event;

namespace {

egraph::RunEvents events() {
    return {"run1",
            {{.cpv = "app-misc/a-1", .uninstall = false},
             {.cpv = "app-misc/old-1", .uninstall = true},
             {.cpv = "app-misc/b-1", .uninstall = false}}};
}

// The field's value, if the event has it.
template <class Value> std::optional<Value> field(const Event& event, std::string_view name) {
    for (const auto& each : event.fields) {
        if (each.name == name) {
            if (const auto* value = std::get_if<Value>(&each.value)) {
                return *value;
            }
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a run's first event says what it will do") {
    auto run = events();
    const auto event = run.started({.command = "exec",
                                    .targets = {"app-misc/a", "app-misc/b"},
                                    .options = {"--oneshot"},
                                    .jobs = 2,
                                    .keep_going = true},
                                   100);
    CHECK(event.kind == "run");
    CHECK(event.run == "run1");
    CHECK(event.time == 100);
    CHECK(event.message == "egraph exec: 2 merges, 1 uninstall, for app-misc/a app-misc/b");
    CHECK(field<std::string>(event, "targets") == "app-misc/a app-misc/b");
    CHECK(field<std::string>(event, "options") == "--oneshot");
    CHECK(field<std::int64_t>(event, "jobs") == 2);
    CHECK(field<std::int64_t>(event, "keep_going") == 1);
    const auto unlimited = events().started({.command = "exec"}, 0);
    CHECK_FALSE(field<std::int64_t>(unlimited, "jobs"));
}

TEST_CASE("a merge logs its phases, its build and its merge, with their times") {
    auto run = events();
    std::ignore = run.started({.command = "exec"}, 0);
    CHECK_FALSE(run.traced(0, Traced::build_started, 10));
    const auto phase = run.reported(0, {.kind = WorkerEvent::Kind::phase, .text = "compile"}, 11);
    REQUIRE(phase);
    CHECK(phase->kind == "phase");
    CHECK(phase->message == "app-misc/a-1: compile");
    CHECK(field<std::int64_t>(*phase, "step") == 1);
    CHECK(field<std::string>(*phase, "phase") == "compile");
    const auto built = run.traced(0, Traced::built, 22.5);
    REQUIRE(built);
    CHECK(built->message == "app-misc/a-1: built in 12.5 s");
    CHECK(field<double>(*built, "seconds") == 12.5);
    CHECK_FALSE(run.traced(0, Traced::merge_started, 30));
    const auto merged = run.traced(0, Traced::merged, 32);
    REQUIRE(merged);
    CHECK(merged->kind == "merged");
    CHECK(field<double>(*merged, "seconds") == 2);
    CHECK(field<double>(*merged, "build_seconds") == 12.5);
    CHECK(field<double>(*merged, "waited") == 7.5);
    // The worker's own word of it adds nothing.
    CHECK_FALSE(run.reported(0, {.kind = WorkerEvent::Kind::merged, .text = "app-misc/a-1"}, 32));
}

TEST_CASE("an uninstall logs as one") {
    auto run = events();
    CHECK_FALSE(run.traced(1, Traced::merge_started, 5));
    const auto gone = run.traced(1, Traced::merged, 6);
    REQUIRE(gone);
    CHECK(gone->kind == "uninstalled");
    CHECK(gone->message == "app-misc/old-1: uninstalled in 1.0 s");
}

TEST_CASE("a failure logs its phase, status and log as an error") {
    auto run = events();
    std::ignore = run.traced(2, Traced::build_started, 0);
    CHECK_FALSE(run.reported(
        2, {.kind = WorkerEvent::Kind::failed, .text = "compile", .status = 1, .log = "/b.log"},
        65));
    const auto failed = run.traced(2, Traced::build_failed, 65);
    REQUIRE(failed);
    CHECK(failed->kind == "failed");
    CHECK(failed->priority == 3);
    CHECK(failed->message == "app-misc/b-1: compile failed with status 1 (log: /b.log)");
    CHECK(field<std::string>(*failed, "phase") == "compile");
    CHECK(field<std::int64_t>(*failed, "status") == 1);
    CHECK(field<std::string>(*failed, "log") == "/b.log");
    CHECK(field<double>(*failed, "seconds") == 65);
    // A request that could not be tried says why.
    auto refused = events();
    std::ignore = refused.reported(
        0, {.kind = WorkerEvent::Kind::error, .text = "no ebuild of app-misc/a-1::x"}, 1);
    const auto error = refused.traced(0, Traced::build_failed, 1);
    REQUIRE(error);
    CHECK(field<std::string>(*error, "error") == "no ebuild of app-misc/a-1::x");
}

TEST_CASE("a skip says why, as a warning, and the end counts everything") {
    auto run = events();
    std::ignore = run.started({.command = "exec"}, 0);
    std::ignore = run.traced(0, Traced::build_started, 0);
    std::ignore = run.traced(0, Traced::build_failed, 1);
    const auto skipped = run.skipped(2, "needs app-misc/a", 2);
    CHECK(skipped.priority == 4);
    CHECK(skipped.message == "app-misc/b-1: skipped, needs app-misc/a");
    CHECK(field<std::string>(skipped, "why") == "needs app-misc/a");
    CHECK_FALSE(run.traced(2, Traced::skipped, 2));
    std::ignore = run.traced(1, Traced::merge_started, 3);
    std::ignore = run.traced(1, Traced::merged, 4);
    const auto end = run.ended("app-misc/a-1: failed", 125);
    CHECK(end.kind == "end");
    CHECK(end.priority == 3);
    CHECK(end.message == "egraph exec failed: 0 merged, 1 uninstalled, 1 failed, 1 skipped in "
                         "2 min 5 s: app-misc/a-1: failed");
    CHECK(field<std::string>(end, "status") == "failed");
    CHECK(field<std::int64_t>(end, "failed") == 1);
    CHECK(field<double>(end, "seconds") == 125);
    CHECK(run.ended(std::nullopt, 4000).message ==
          "egraph exec done: 0 merged, 1 uninstalled, 1 failed, 1 skipped in 1 h 6 min");
}

TEST_CASE("a command handing over to emerge logs the command line and how it exited") {
    const egraph::HandOver install{.command = "install",
                                   .targets = {"app-misc/a", "app-misc/b"},
                                   .program = "emerge",
                                   .argv = {"emerge", "--ask=n", "app-misc/a", "app-misc/b"}};
    const auto start = egraph::handed_over("run1", install, 100);
    CHECK(start.kind == "run");
    CHECK(start.run == "run1");
    CHECK(start.message == "egraph install: emerge, for app-misc/a app-misc/b");
    CHECK(field<std::string>(start, "command") == "install");
    CHECK(field<std::string>(start, "targets") == "app-misc/a app-misc/b");
    CHECK(field<std::string>(start, "program") == "emerge");
    CHECK(field<std::string>(start, "argv") == "emerge --ask=n app-misc/a app-misc/b");
    CHECK(egraph::handed_over("run2", {.command = "sync", .program = "emaint"}, 0).message ==
          "egraph sync: emaint");

    const auto done = egraph::handed_back("run1", install, 0, 100, 112.5);
    CHECK(done.kind == "end");
    CHECK(done.priority == 6);
    CHECK(done.message == "egraph install done in 12.5 s");
    CHECK(field<std::string>(done, "status") == "ok");
    CHECK(field<std::int64_t>(done, "exit_status") == 0);
    CHECK(field<double>(done, "seconds") == 12.5);
    CHECK_FALSE(field<std::string>(done, "error"));

    const auto exited = egraph::handed_back("run1", install, 1, 100, 103);
    CHECK(exited.priority == 3);
    CHECK(exited.message == "egraph install failed in 3.0 s: emerge exited with status 1");
    CHECK(field<std::string>(exited, "status") == "failed");
    CHECK(field<std::int64_t>(exited, "exit_status") == 1);
    CHECK(field<std::string>(exited, "error") == "emerge exited with status 1");

    const auto unrun = egraph::handed_back(
        "run1", install, std::unexpected(std::string{"emerge: not found"}), 100, 100);
    CHECK(unrun.message == "egraph install failed in 0.0 s: emerge: not found");
    CHECK_FALSE(field<std::int64_t>(unrun, "exit_status"));
    CHECK(field<std::string>(unrun, "error") == "emerge: not found");
}
