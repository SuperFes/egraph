#include "schedule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace {

egraph::Merge merge_waiting_for(std::vector<std::uint32_t> waited) {
    egraph::Merge merge;
    for (const auto other : waited) {
        egraph::WaitKinds kinds;
        kinds.run = true;
        merge.waits.push_back({.merge = other, .kinds = kinds});
    }
    return merge;
}

// A plan of merges in order, each waiting for the merges listed, and its steps.
struct Planned {
    egraph::Plan plan;
    std::vector<egraph::Step> steps;
};

Planned planned(const std::vector<std::vector<std::uint32_t>>& waits) {
    Planned found;
    for (std::uint32_t i = 0; i < waits.size(); ++i) {
        found.plan.merges.push_back(merge_waiting_for(waits.at(i)));
        found.plan.order.push_back(i);
        found.steps.emplace_back(egraph::MergeStep{.merge = i, .blockers = {}, .world = {}});
    }
    return found;
}

void build(egraph::Schedule& schedule, std::size_t step) {
    REQUIRE(schedule.next_build() == step);
    schedule.build_started(step);
}

void merge(egraph::Schedule& schedule, std::size_t step) {
    REQUIRE(schedule.next_merge() == step);
    schedule.merge_started(step);
    schedule.merge_finished(step, true);
}

} // namespace

TEST_CASE("builds run beside each other up to the jobs, and merge once none runs") {
    auto [plan, steps] = planned({{}, {}, {}});
    egraph::Schedule schedule{plan, steps, 2};
    build(schedule, 0);
    build(schedule, 1);
    CHECK(schedule.next_build() == std::nullopt);
    schedule.build_finished(0, true);
    // merge-wait: not while a build runs.
    CHECK(schedule.next_merge() == std::nullopt);
    build(schedule, 2);
    schedule.build_finished(2, true);
    schedule.build_finished(1, true);
    // In the order the builds finished, one at a time.
    REQUIRE(schedule.next_merge() == 0);
    schedule.merge_started(0);
    CHECK(schedule.next_merge() == std::nullopt);
    schedule.merge_finished(0, true);
    merge(schedule, 2);
    merge(schedule, 1);
    CHECK(schedule.finished());
    CHECK_FALSE(schedule.failed());
}

TEST_CASE("no build starts while what built waits to merge") {
    auto [plan, steps] = planned({{}, {}});
    egraph::Schedule schedule{plan, steps, 1};
    build(schedule, 0);
    schedule.build_finished(0, true);
    // As emerge holds new jobs while merge-wait's merges are scheduled.
    CHECK(schedule.next_build() == std::nullopt);
    merge(schedule, 0);
    build(schedule, 1);
}

TEST_CASE("a build waits for the merges it reaches, but those after it") {
    // 1 waits for 0; 2 waits for 3, after it, which waits for 0.
    auto [plan, steps] = planned({{}, {0}, {3}, {0}});
    egraph::Schedule schedule{plan, steps, std::nullopt};
    build(schedule, 0);
    CHECK(schedule.next_build() == std::nullopt);
    schedule.build_finished(0, true);
    merge(schedule, 0);
    build(schedule, 1);
    build(schedule, 2);
    build(schedule, 3);
}

TEST_CASE("a merge waited for through a merge already done still holds a build back") {
    // 2 waits for 1, done, which waits for 0, not yet.
    auto [plan, steps] = planned({{}, {}, {1}});
    plan.merges.at(1) = merge_waiting_for({0});
    plan.order = {1, 0, 2};
    steps = {egraph::MergeStep{.merge = 1, .blockers = {}, .world = {}},
             egraph::MergeStep{.merge = 0, .blockers = {}, .world = {}},
             egraph::MergeStep{.merge = 2, .blockers = {}, .world = {}}};
    egraph::Schedule schedule{plan, steps, std::nullopt};
    // 1 waits for 0, after it: a broken cycle.
    build(schedule, 0);
    schedule.build_finished(0, true);
    merge(schedule, 0);
    build(schedule, 1);
    CHECK(schedule.next_build() == std::nullopt);
    schedule.build_finished(1, true);
    merge(schedule, 1);
    build(schedule, 2);
}

TEST_CASE("an uninstall goes first once the merges it waits for are done") {
    auto [plan, steps] = planned({{}, {}});
    plan.uninstalls.push_back({.package = 0, .why = {}, .after = {0}});
    steps.insert(steps.begin() + 1, egraph::UninstallStep{.uninstall = 0, .clean_world = false});
    egraph::Schedule schedule{plan, steps, std::nullopt};
    build(schedule, 0);
    build(schedule, 2);
    CHECK(schedule.next_build() == std::nullopt);
    schedule.build_finished(0, true);
    schedule.build_finished(2, true);
    merge(schedule, 0);
    // Ahead of the merge waiting since before.
    merge(schedule, 1);
    merge(schedule, 2);
    CHECK(schedule.finished());
}

TEST_CASE("after a failure nothing new builds, and what built still merges") {
    auto [plan, steps] = planned({{}, {}, {}});
    egraph::Schedule schedule{plan, steps, 2};
    build(schedule, 0);
    build(schedule, 1);
    schedule.build_finished(0, false);
    CHECK(schedule.failed());
    CHECK(schedule.next_build() == std::nullopt);
    CHECK_FALSE(schedule.finished());
    schedule.build_finished(1, true);
    merge(schedule, 1);
    CHECK(schedule.finished());
}

TEST_CASE("a failed merge stops the run as a failed build does") {
    auto [plan, steps] = planned({{}, {}});
    egraph::Schedule schedule{plan, steps, 1};
    build(schedule, 0);
    schedule.build_finished(0, true);
    REQUIRE(schedule.next_merge() == 0);
    schedule.merge_started(0);
    schedule.merge_finished(0, false);
    CHECK(schedule.failed());
    CHECK(schedule.next_build() == std::nullopt);
    CHECK(schedule.finished());
}
