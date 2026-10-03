#include "observe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using egraph::emerge::Task;
using egraph::emerge::TaskKind;

namespace {

// Merges a and b, then the uninstall of old once a has merged.
struct Run {
    egraph::Plan plan;
    std::vector<egraph::Step> steps;
};

Run run() {
    Run found;
    found.plan.merges.resize(2);
    found.plan.order = {0, 1};
    found.plan.uninstalls = {{.package = 0, .why = {}, .after = {0}}};
    found.steps = {egraph::MergeStep{.merge = 0, .blockers = {}, .world = {}},
                   egraph::MergeStep{.merge = 1, .blockers = {}, .world = {}},
                   egraph::UninstallStep{.uninstall = 0, .clean_world = false}};
    return found;
}

std::vector<egraph::Observed> named() {
    return {{.cpv = "app-misc/a-1", .root = "/", .operation = "merge"},
            {.cpv = "app-misc/b-1", .root = "/", .operation = "merge"},
            {.cpv = "app-misc/old-1", .root = "/", .operation = "uninstall"}};
}

} // namespace

TEST_CASE("a snapshot reports each step as emerge's scheduler does, from build to merge") {
    const auto [plan, steps] = run();
    egraph::Schedule schedule{plan, steps, 2};
    egraph::Observer observer{4321, 2, named()};

    schedule.build_started(0);
    observer.update(schedule, 100);
    observer.worker(0, 501);
    observer.phase(0, "setup");
    schedule.build_started(1);
    observer.update(schedule, 101);
    observer.worker(1, 502);
    observer.phase(1, "compile");
    schedule.build_finished(0, true);
    observer.update(schedule, 110);

    auto snapshot = observer.snapshot(schedule, 112);
    CHECK(snapshot.pid == 4321);
    CHECK(snapshot.timestamp == 112);
    CHECK(snapshot.jobs == egraph::emerge::Jobs{.running = 1,
                                                .max = 2,
                                                .completed = 0,
                                                .total = 2,
                                                .failed = 0,
                                                .merge_wait = 1,
                                                .merges_pending = 0});
    // Waiting for merge-wait: a merge, its time frozen when its build ended.
    CHECK(snapshot.tasks == std::vector<Task>{{.cpv = "app-misc/a-1",
                                               .root = "/",
                                               .operation = "merge",
                                               .kind = TaskKind::merge,
                                               .phase = "merge-wait",
                                               .merge_wait = true,
                                               .start_time = 100,
                                               .elapsed = 10,
                                               .build_elapsed = 10},
                                              {.cpv = "app-misc/b-1",
                                               .root = "/",
                                               .operation = "merge",
                                               .kind = TaskKind::build,
                                               .phase = "compile",
                                               .pid = 502,
                                               .start_time = 101,
                                               .elapsed = 11,
                                               .build_elapsed = 11}});

    schedule.build_finished(1, true);
    observer.update(schedule, 120);
    schedule.merge_started(0);
    observer.update(schedule, 121);
    observer.worker(0, 501);
    observer.phase(0, "preinst");
    snapshot = observer.snapshot(schedule, 122);
    CHECK(snapshot.jobs.running == 0);
    CHECK(snapshot.jobs.merge_wait == 0);
    CHECK(snapshot.jobs.merges_pending == 2);
    // Let through, a merge keeps the last phase its build reported, and runs on no worker yet.
    CHECK(snapshot.tasks == std::vector<Task>{{.cpv = "app-misc/a-1",
                                               .root = "/",
                                               .operation = "merge",
                                               .kind = TaskKind::merge,
                                               .phase = "preinst",
                                               .pid = 501,
                                               .start_time = 100,
                                               .elapsed = 22,
                                               .build_elapsed = 10},
                                              {.cpv = "app-misc/b-1",
                                               .root = "/",
                                               .operation = "merge",
                                               .kind = TaskKind::merge,
                                               .phase = "compile",
                                               .start_time = 101,
                                               .elapsed = 21,
                                               .build_elapsed = 19}});

    schedule.merge_finished(0, true);
    observer.update(schedule, 130);
    snapshot = observer.snapshot(schedule, 131);
    CHECK(snapshot.jobs.completed == 1);
    REQUIRE(snapshot.tasks.size() == 2);
    // An uninstall starts once let through, and has no build.
    CHECK(snapshot.tasks.at(1) == Task{.cpv = "app-misc/old-1",
                                       .root = "/",
                                       .operation = "uninstall",
                                       .kind = TaskKind::merge,
                                       .start_time = 130,
                                       .elapsed = 1});

    schedule.merge_started(2);
    schedule.merge_finished(2, false);
    schedule.merge_started(1);
    schedule.merge_finished(1, true);
    observer.update(schedule, 140);
    snapshot = observer.snapshot(schedule, 140);
    CHECK(snapshot.jobs.completed == 2);
    CHECK(snapshot.jobs.failed == 1);
    CHECK(snapshot.tasks.empty());
}

TEST_CASE("a run without a job limit reports no maximum") {
    const auto [plan, steps] = run();
    const egraph::Schedule schedule{plan, steps, std::nullopt};
    const egraph::Observer observer{1, std::nullopt, named()};
    CHECK_FALSE(observer.snapshot(schedule, 0).jobs.max.has_value());
}

TEST_CASE("once keep-going goes on, the total is the merges left") {
    egraph::Plan plan;
    plan.merges.resize(3);
    plan.order = {0, 1, 2};
    std::vector<egraph::Step> steps;
    std::vector<egraph::Observed> names;
    for (std::uint32_t i = 0; i < 3; ++i) {
        steps.emplace_back(egraph::MergeStep{.merge = i, .blockers = {}, .world = {}});
        names.push_back(
            {.cpv = "app-misc/m-" + std::to_string(i), .root = "/", .operation = "merge"});
    }
    egraph::Schedule schedule{plan, steps, 1};
    egraph::Observer observer{1, 1, names};
    CHECK(observer.snapshot(schedule, 0).jobs.total == 3);
    schedule.build_started(0);
    schedule.build_finished(0, false);
    schedule.resume(std::array<std::size_t, 1>{1});
    observer.update(schedule, 1);
    observer.resumed(schedule);
    const auto snapshot = observer.snapshot(schedule, 1);
    CHECK(snapshot.jobs.total == 1);
    CHECK(snapshot.jobs.failed == 1);
}
