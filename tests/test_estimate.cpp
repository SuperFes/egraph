#include "estimate.hpp"
#include "index_builder.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using Catch::Approx;
using egraph::BuildKnowledge;
using egraph::BuildRecord;
using egraph::BuildWanted;
using egraph::PackageEstimate;
using egraph::PlanEstimate;
using egraph::Schedule;
using egraph::StepCost;

namespace {

BuildRecord build(std::string cpv, double seconds, std::optional<double> cpu = std::nullopt) {
    return {.origin = "exec",
            .run = "run",
            .cpv = std::move(cpv),
            .phases = {{.phase = "compile", .seconds = seconds}},
            .build_seconds = seconds,
            .merge_seconds = 2,
            .cpu_seconds = cpu};
}

BuildRecord beside(BuildRecord record, std::uint32_t others, std::optional<double> busy) {
    record.beside = others;
    if (busy) {
        record.system = egraph::SystemShare{.cpu_busy = busy};
    }
    return record;
}

// A plan of merges in order, each waiting for the merges listed, and its steps.
struct Planned {
    egraph::Plan plan;
    std::vector<egraph::Step> steps;
};

Planned planned(const std::vector<std::vector<std::uint32_t>>& waits) {
    Planned found;
    for (std::uint32_t i = 0; i < waits.size(); ++i) {
        egraph::Merge merge;
        for (const auto other : waits.at(i)) {
            egraph::WaitKinds kinds;
            kinds.run = true;
            merge.waits.push_back({.merge = other, .kinds = kinds});
        }
        found.plan.merges.push_back(std::move(merge));
        found.plan.order.push_back(i);
        found.steps.emplace_back(egraph::MergeStep{.merge = i, .blockers = {}, .world = {}});
    }
    return found;
}

StepCost cost(double seconds, double parallelism = 1,
              std::optional<std::uint64_t> memory = std::nullopt) {
    return {.build_seconds = seconds,
            .parallelism = parallelism,
            .peak_memory = memory,
            .merge_seconds = 1,
            .known = true};
}

} // namespace

TEST_CASE("a package never merged here has no estimate") {
    auto failed = build("app-misc/a-1", 30, 30);
    failed.outcome = BuildRecord::Outcome::failed;
    const BuildKnowledge knowledge{{failed, build("app-misc/b-1", 10)}};
    CHECK_FALSE(knowledge.empty());
    CHECK_FALSE(knowledge.estimate("app-misc/a-1", 8));
    CHECK_FALSE(knowledge.estimate("app-misc/c-1", 8));
    CHECK(BuildKnowledge{{failed}}.empty());
}

TEST_CASE("a build alone is timed as it took, its fetch left out") {
    auto alone = build("app-misc/a-1", 100, 300);
    alone.phases.insert(alone.phases.begin(), {.phase = "fetch", .seconds = 20});
    alone.build_seconds = 120;
    alone.peak_memory = 1000;
    auto again = build("app-misc/a-1", 110, 330);
    again.peak_memory = 3000;
    again.merge_seconds = 4;
    const BuildKnowledge knowledge{{alone, again}};
    CHECK(knowledge.estimate("app-misc/a-1", 8) == PackageEstimate{.seconds = 105,
                                                                   .parallelism = 3,
                                                                   .peak_memory = 3000,
                                                                   .merge_seconds = 3,
                                                                   .builds = 2,
                                                                   .same_version = true});
}

TEST_CASE("the version's own builds come first, then the cp's latest") {
    std::vector<BuildRecord> records;
    records.reserve(9);
    for (int i = 0; i < 7; ++i) {
        records.push_back(build("app-misc/a-1", 10.0 * (i + 1)));
    }
    records.push_back(build("app-misc/a-2", 500));
    records.push_back(build("app-misc/a-1", 1000));
    const BuildKnowledge knowledge{records};
    // a-2's one build.
    const auto two = knowledge.estimate("app-misc/a-2", 4);
    REQUIRE(two);
    CHECK(two->seconds == 500);
    CHECK(two->builds == 1);
    CHECK(two->same_version);
    // a-1's latest five: 40, 50, 60, 70 and 1000.
    const auto one = knowledge.estimate("app-misc/a-1", 4);
    REQUIRE(one);
    CHECK(one->seconds == 60);
    CHECK(one->builds == 5);
    // a-3 from the cp's latest five: 50, 60, 70, 500 and 1000.
    const auto three = knowledge.estimate("app-misc/a-3", 4);
    REQUIRE(three);
    CHECK(three->seconds == 70);
    CHECK_FALSE(three->same_version);
    // Without CPU seconds, one core's worth.
    CHECK(three->parallelism == 1);
}

TEST_CASE("a build starved beside others is timed by its CPU seconds and its parallelism") {
    // Alone it kept 4 cores busy; beside two others on a saturated system it took 300 s for
    // 420 CPU seconds, 105 s at its own pace.
    const BuildKnowledge knowledge{
        {build("app-misc/a-1", 100, 400), beside(build("app-misc/a-1", 300, 420), 2, 1.0)}};
    const auto estimate = knowledge.estimate("app-misc/a-1", 16);
    REQUIRE(estimate);
    CHECK(estimate->seconds == Approx(102.5));
    CHECK(estimate->parallelism == Approx(410 / 102.5));
    // Beside others on a system with room, it took what it needed.
    const BuildKnowledge roomy{
        {build("app-misc/a-1", 100, 400), beside(build("app-misc/a-1", 300, 420), 2, 0.5)}};
    CHECK(roomy.estimate("app-misc/a-1", 16)->seconds == Approx(200));
    // emerge's builds say nothing of the system: beside others, it counts as starved.
    const BuildKnowledge unsaid{{build("app-misc/a-1", 100, 400),
                                 beside(build("app-misc/a-1", 300, 420), 2, std::nullopt)}};
    CHECK(unsaid.estimate("app-misc/a-1", 16)->seconds == Approx(102.5));
    // Starved without CPU seconds: as it took.
    const BuildKnowledge blind{{beside(build("app-misc/a-1", 300), 2, 1.0)}};
    CHECK(blind.estimate("app-misc/a-1", 16)->seconds == 300);
}

TEST_CASE("a package's parallelism is no more than the system's cores") {
    const BuildKnowledge knowledge{{build("app-misc/a-1", 100, 800)}};
    CHECK(knowledge.estimate("app-misc/a-1", 16)->parallelism == 8);
    CHECK(knowledge.estimate("app-misc/a-1", 4)->parallelism == 4);
    // Waiting on nothing but sleep.
    const BuildKnowledge idle{{build("app-misc/a-1", 100, 0)}};
    const auto estimate = idle.estimate("app-misc/a-1", 4);
    CHECK(estimate->seconds == 100);
    CHECK(estimate->parallelism == 0);
}

TEST_CASE("the builds of the nearest USE come first, then the latest") {
    std::vector<BuildRecord> records;
    for (int i = 0; i < 3; ++i) {
        auto record = build("app-misc/a-1", 100);
        record.use = {"x"};
        records.push_back(record);
    }
    for (int i = 0; i < 5; ++i) {
        auto record = build("app-misc/a-1", 200);
        record.use = {"x", "y"};
        records.push_back(record);
    }
    const BuildKnowledge knowledge{records};
    const auto seconds = [&](std::optional<std::vector<std::string>> use) {
        return knowledge.estimate(BuildWanted{.cpv = "app-misc/a-1", .use = std::move(use)}, 8)
            ->seconds;
    };
    // Three of the same USE and two a flag away.
    CHECK(seconds(std::vector<std::string>{"x"}) == 100);
    CHECK(seconds(std::vector<std::string>{"x", "y"}) == 200);
    CHECK(seconds(std::vector<std::string>{"x", "z"}) == 100);
    // No USE known: the latest.
    CHECK(seconds(std::nullopt) == 200);
}

TEST_CASE("a build's time scales by the wanted distfiles against its own, within a limit") {
    auto record = build("app-misc/a-1", 100, 200);
    record.distfiles_size = 1000;
    const BuildKnowledge knowledge{{record}};
    const auto scaled = [&](std::optional<std::uint64_t> distfiles) {
        return *knowledge.estimate(BuildWanted{.cpv = "app-misc/a-2", .distfiles = distfiles}, 8);
    };
    CHECK(scaled(1500).seconds == Approx(150));
    // Its parallelism stays the package's.
    CHECK(scaled(1500).parallelism == Approx(2));
    CHECK(scaled(5000).seconds == Approx(200));
    CHECK(scaled(100).seconds == Approx(50));
    CHECK(scaled(std::nullopt).seconds == 100);
    // Nor is a build that never said its distfiles' size scaled.
    const BuildKnowledge unsized{{build("app-misc/a-1", 100)}};
    CHECK(unsized.estimate(BuildWanted{.cpv = "app-misc/a-2", .distfiles = 1500}, 8)->seconds ==
          100);
}

TEST_CASE("a plan's builds that wait for each other add up, merges and all") {
    const auto [plan, steps] = planned({{}, {0}, {1}});
    const std::vector costs{cost(10), cost(20), cost(30)};
    CHECK(egraph::estimate_plan(Schedule{plan, steps, std::nullopt}, costs, 4) ==
          PlanEstimate{.seconds = 63, .known = 3, .finished = {11, 32, 63}});
}

TEST_CASE("builds beside each other take the longest, merging once none runs") {
    const auto [plan, steps] = planned({{}, {}, {}});
    const std::vector costs{cost(10, 2, 100), cost(20, 2, 200), cost(30, 2)};
    const auto estimate = egraph::estimate_plan(Schedule{plan, steps, 3}, costs, 8);
    CHECK(estimate.seconds == 33);
    CHECK(estimate.finished == std::vector<std::optional<double>>{31, 32, 33});
    // The peaks known, of the builds at once.
    CHECK(estimate.peak_memory == 300U);
    // Without merge-wait, each merges once built.
    const auto eager =
        egraph::estimate_plan(Schedule{plan, steps, 3, {.feature = false}}, costs, 8);
    CHECK(eager.finished == std::vector<std::optional<double>>{11, 21, 31});
    // One at a time.
    CHECK(egraph::estimate_plan(Schedule{plan, steps, 1}, costs, 8).seconds == 63);
}

TEST_CASE("builds wanting more cores than the system has share them") {
    const auto [plan, steps] = planned({{}, {}});
    const std::vector costs{cost(10, 4), cost(10, 4)};
    CHECK(egraph::estimate_plan(Schedule{plan, steps, 2}, costs, 4).seconds == Approx(22));
    CHECK(egraph::estimate_plan(Schedule{plan, steps, 2}, costs, 8).seconds == Approx(12));
    // A build that sleeps keeps its pace.
    const std::vector sleepy{cost(10, 4), cost(10, 4), cost(10, 0)};
    const auto [three, three_steps] = planned({{}, {}, {}});
    const auto estimate =
        egraph::estimate_plan(Schedule{three, three_steps, 3, {.feature = false}}, sleepy, 4);
    CHECK(estimate.finished.at(2) == Approx(11));
}

TEST_CASE("a jobserver's tokens hold builds back unless one would run alone") {
    const auto [plan, steps] = planned({{}, {}});
    const std::vector costs{cost(10, 1, 100), cost(10, 1, 200)};
    const auto gated = egraph::estimate_plan(Schedule{plan, steps, std::nullopt}, costs, 4, 1);
    CHECK(gated.seconds == 22);
    CHECK(gated.peak_memory == 200U);
    CHECK(egraph::estimate_plan(Schedule{plan, steps, std::nullopt}, costs, 4, 2).seconds == 12);
    CHECK(egraph::estimate_plan(Schedule{plan, steps, std::nullopt}, costs, 4, 0).seconds == 22);
}

TEST_CASE("steps never built here cost nothing and are counted apart") {
    const auto [plan, steps] = planned({{}, {0}});
    const std::vector costs{StepCost{}, cost(10)};
    const auto estimate = egraph::estimate_plan(Schedule{plan, steps, 2}, costs, 4);
    CHECK(estimate.seconds == 11);
    CHECK(estimate.known == 1);
    CHECK(estimate.unknown == 1);
    CHECK_FALSE(estimate.peak_memory);
}

TEST_CASE("a run under way is estimated from where it stands") {
    const auto [plan, steps] = planned({{}, {0}, {1}});
    const std::vector costs{cost(10), cost(20), cost(30)};
    Schedule schedule{plan, steps, 2};
    REQUIRE(schedule.next_build() == 0);
    schedule.build_started(0);
    schedule.build_finished(0, true);
    REQUIRE(schedule.next_merge() == 0);
    schedule.merge_started(0);
    schedule.merge_finished(0, true);
    REQUIRE(schedule.next_build() == 1);
    schedule.build_started(1);
    const std::vector<egraph::RunningStep> running{{.step = 1, .merging = false, .elapsed = 15}};
    const auto estimate = egraph::estimate_plan(schedule, costs, 4, std::nullopt, running);
    CHECK(estimate == PlanEstimate{.seconds = 37, .known = 2, .finished = {std::nullopt, 6, 37}});
    // Past its estimate: done any moment.
    const std::vector<egraph::RunningStep> late{{.step = 1, .merging = false, .elapsed = 50}};
    CHECK(egraph::estimate_plan(schedule, costs, 4, std::nullopt, late).finished.at(1) == 1);
    // Merging.
    schedule.build_finished(1, true);
    REQUIRE(schedule.next_merge() == 1);
    schedule.merge_started(1);
    const std::vector<egraph::RunningStep> merging{{.step = 1, .merging = true, .elapsed = 0.5}};
    CHECK(egraph::estimate_plan(schedule, costs, 4, std::nullopt, merging).seconds == 31.5);
}

TEST_CASE("a plan's steps cost what their packages' builds foretell") {
    auto known = build("app-misc/a-1", 100, 200);
    known.peak_memory = 5;
    known.jobs = 4;
    auto last = build("app-misc/z-1", 10);
    last.jobs = std::nullopt;
    const BuildKnowledge knowledge{{known, last}};
    const std::vector<egraph::BuildStep> steps{{.cpv = "app-misc/a-1"},
                                               {.cpv = "app-misc/b-1"},
                                               {.cpv = "app-misc/a-0", .uninstall = true}};
    CHECK(egraph::step_costs(knowledge, steps, 8) == std::vector<StepCost>{{.build_seconds = 100,
                                                                            .parallelism = 2,
                                                                            .peak_memory = 5,
                                                                            .merge_seconds = 2,
                                                                            .known = true},
                                                                           {},
                                                                           {}});
    // With the index's facts, built with the step's USE.
    auto sized = build("app-misc/a-1", 100);
    sized.distfiles_size = 100;
    egraph::test::IndexBuilder builder;
    builder.version({.cpv = "app-misc/a-2",
                     .fetch = {{.places = {""}, .bytes = 100}, {.places = {"doc?"}, .bytes = 50}}});
    const std::vector<egraph::BuildStep> wanted{
        {.cpv = "app-misc/a-2", .repo = "gentoo", .use = {"doc"}},
        {.cpv = "app-misc/a-2", .repo = "overlay", .use = {"doc"}},
        {.cpv = "app-misc/a-2", .repo = "gentoo", .uninstall = true}};
    const auto facts = egraph::step_facts(builder.index(), wanted);
    CHECK(facts == std::vector<egraph::StepFacts>{{.distfiles = 150}, {}, {}});
    CHECK(egraph::step_costs(BuildKnowledge{{sized}}, wanted, 8, facts).at(0).build_seconds ==
          Approx(150));
    CHECK_FALSE(egraph::recorded_jobs({known, last}));
    CHECK(egraph::recorded_jobs({last, known}) == 4U);
    CHECK_FALSE(egraph::recorded_jobs({}));
}

TEST_CASE("a plan's estimate is a line for scripts") {
    CHECK(egraph::estimate_line(
              {.seconds = 7810.4, .peak_memory = 6000000000, .known = 30, .unknown = 4}, 4) ==
          "\testimate\t7810\t6000000000\t30\t4\t4");
    CHECK(egraph::estimate_line({.seconds = 40.6, .known = 1}, std::nullopt) ==
          "\testimate\t41\t\t1\t0\t");
}

TEST_CASE("an estimate line is read back") {
    const PlanEstimate estimate{.seconds = 7810, .peak_memory = 6, .known = 30, .unknown = 4};
    CHECK(egraph::parse_estimate_line(egraph::estimate_line(estimate, 4)) ==
          egraph::EstimateLine{
              .seconds = 7810, .peak_memory = 6, .known = 30, .unknown = 4, .jobs = 4});
    CHECK(egraph::parse_estimate_line("\testimate\t41\t\t1\t0\t") ==
          egraph::EstimateLine{.seconds = 41, .known = 1});
    CHECK_FALSE(egraph::parse_estimate_line("\ttried\tnone"));
    CHECK_FALSE(egraph::parse_estimate_line("\testimate\tx\t\t1\t0\t"));
    CHECK_FALSE(egraph::parse_estimate_line("\testimate\t41\t\t1\t0"));
}

TEST_CASE("a run counts down, estimated again as its steps start and end") {
    using egraph::Traced;
    const auto [plan, steps] = planned({{}, {0}});
    const std::vector costs{cost(10), cost(20)};
    Schedule schedule{plan, steps, 2};
    egraph::Countdown countdown{costs, 4, std::nullopt};
    CHECK(countdown.left(schedule, 100) == 32);
    REQUIRE(schedule.next_build() == 0);
    schedule.build_started(0);
    countdown.traced(0, Traced::build_started, 100);
    CHECK(countdown.left(schedule, 104) == 28);
    // Counted down between events.
    CHECK(countdown.left(schedule, 106) == 26);
    // Built 5 s later than foretold, then merging.
    schedule.build_finished(0, true);
    countdown.traced(0, Traced::built, 115);
    REQUIRE(schedule.next_merge() == 0);
    schedule.merge_started(0);
    countdown.traced(0, Traced::merge_started, 115);
    CHECK(countdown.left(schedule, 115) == 22);
    CHECK(countdown.left(schedule, 130) == 7);
    CHECK(countdown.left(schedule, 200) == 0);
    // Nothing left known.
    egraph::Countdown blind{std::vector<StepCost>(2), 4, std::nullopt};
    CHECK_FALSE(blind.left(schedule, 115));
}

TEST_CASE("a status display's build tasks are given their expected seconds") {
    const BuildKnowledge knowledge{{build("app-misc/a-1", 100, 200)}};
    using egraph::emerge::Task;
    using egraph::emerge::TaskKind;
    std::vector<egraph::emerge::Snapshot> snapshots{
        {.pid = 1,
         .tasks = {{.cpv = "app-misc/a-2", .operation = "merge", .kind = TaskKind::build},
                   {.cpv = "app-misc/b-1", .operation = "merge", .kind = TaskKind::build},
                   {.cpv = "app-misc/a-1", .operation = "merge", .binary = true},
                   {.cpv = "app-misc/a-1", .operation = "uninstall", .kind = TaskKind::merge}}},
        {.pid = 2, .tasks = {{.cpv = "app-misc/a-1", .operation = "", .kind = TaskKind::build}}}};
    egraph::expect_builds(snapshots, knowledge, 8);
    CHECK(snapshots.at(0).tasks.at(0).expected == 100);
    CHECK_FALSE(snapshots.at(0).tasks.at(1).expected);
    CHECK_FALSE(snapshots.at(0).tasks.at(2).expected);
    CHECK_FALSE(snapshots.at(0).tasks.at(3).expected);
    CHECK(snapshots.at(1).tasks.at(0).expected == 100);
}
