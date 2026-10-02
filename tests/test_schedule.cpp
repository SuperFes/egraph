#include "schedule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

egraph::Merge merge_waiting_for(const std::vector<std::uint32_t>& waited) {
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

TEST_CASE("a schedule is alone with no build running and none waiting to merge") {
    auto [plan, steps] = planned({{}, {}});
    egraph::Schedule schedule{plan, steps, std::nullopt};
    CHECK(schedule.alone());
    build(schedule, 0);
    CHECK(schedule.building() == 1);
    CHECK_FALSE(schedule.alone());
    build(schedule, 1);
    schedule.build_finished(0, true);
    CHECK(schedule.building() == 1);
    // What built waits to merge.
    CHECK_FALSE(schedule.alone());
    schedule.build_finished(1, true);
    CHECK(schedule.alone());
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

namespace {

// Workers that answer the requests sent to them in the order sent: a build with a phase and then
// built, a merge or uninstall with done, but those set to fail or to end their worker.
struct FakePool {
    // By worker, the lines sent to it.
    std::vector<std::vector<std::string>> sent;
    std::deque<std::pair<std::size_t, std::string>> running;
    std::deque<egraph::Heard> heard;
    std::set<std::string, std::less<>> failing;
    std::set<std::string, std::less<>> ending;
    // Requests refused, or not taken, or answered with what is not an event.
    std::set<std::string, std::less<>> refused;
    std::set<std::string, std::less<>> deaf;
    std::set<std::string, std::less<>> garbled;
    // Free tokens, with a jobserver; and those another holder gives back once asked for.
    std::optional<int> tokens;
    int freed_later = 0;
    // Whether there is room for a job beside those running.
    bool room = true;
    std::vector<std::uint32_t> asked_room;

    bool room_for(std::uint32_t beside) {
        asked_room.push_back(beside);
        return room;
    }

    std::expected<std::size_t, std::string> add() {
        sent.emplace_back();
        return sent.size() - 1;
    }

    bool send(std::size_t worker, std::string_view line) {
        REQUIRE(
            std::ranges::none_of(running, [&](const auto& run) { return run.first == worker; }));
        if (deaf.contains(line)) {
            return false;
        }
        sent.at(worker).emplace_back(line);
        running.emplace_back(worker, line);
        return true;
    }

    std::expected<bool, std::string> take_token(std::size_t /*step*/) {
        if (!tokens) {
            return true;
        }
        if (*tokens == 0) {
            return false;
        }
        --*tokens;
        return true;
    }

    std::expected<void, std::string> give_token(std::size_t /*step*/) {
        if (tokens) {
            ++*tokens;
        }
        return {};
    }

    std::expected<egraph::Heard, std::string> next(bool for_token) {
        if (heard.empty()) {
            if (running.empty()) {
                if (for_token && freed_later > 0) {
                    --freed_later;
                    ++*tokens;
                    return egraph::Heard{.worker = std::nullopt, .line = std::nullopt};
                }
                return std::unexpected("nothing to hear");
            }
            const auto [worker, line] = running.front();
            running.pop_front();
            const auto space = line.find(' ');
            const auto kind = line.substr(0, space);
            const auto name = line.substr(space + 1);
            if (kind == "build") {
                heard.push_back({.worker = worker, .line = R"({"phase": "compile"})"});
            }
            if (refused.contains(line)) {
                heard.push_back({.worker = worker, .line = R"({"error": "no ebuild"})"});
            } else if (garbled.contains(line)) {
                heard.push_back({.worker = worker, .line = "garbage"});
            } else if (ending.contains(line)) {
                heard.push_back({.worker = worker, .line = std::nullopt});
            } else if (failing.contains(line)) {
                heard.push_back({.worker = worker,
                                 .line = R"({"failed": "compile", "status": 1, "log": "/l"})"});
            } else {
                const auto* done = kind == "build"   ? "built"
                                   : kind == "merge" ? "merged"
                                                     : "uninstalled";
                heard.push_back(
                    {.worker = worker, .line = std::format(R"({{"{}": "{}"}})", done, name)});
            }
        }
        auto next = heard.front();
        heard.pop_front();
        return next;
    }
};

// Requests named for their steps: "build a", "merge a", "uninstall b".
struct FakeRequests {
    std::vector<egraph::Step> steps;
    std::vector<std::size_t> done_steps;

    [[nodiscard]] std::string name(std::size_t step) const {
        return std::string(1, static_cast<char>('a' + step));
    }
    [[nodiscard]] std::string build(std::size_t step) const { return "build " + name(step); }
    [[nodiscard]] std::string merge(std::size_t step) const {
        return (std::holds_alternative<egraph::MergeStep>(steps.at(step)) ? "merge "
                                                                          : "uninstall ") +
               name(step);
    }
    void done(std::size_t step) { done_steps.push_back(step); }
};

struct Ran {
    egraph::PoolOutcome outcome;
    std::vector<std::string> trace;
    std::vector<std::string> events;
    std::vector<std::size_t> done;
};

Ran run(const Planned& planned, std::optional<std::uint32_t> jobs, FakePool& pool) {
    egraph::Schedule schedule{planned.plan, planned.steps, jobs};
    FakeRequests requests{.steps = planned.steps, .done_steps = {}};
    Ran ran;
    ran.outcome = egraph::run_schedule(
        schedule, pool, requests,
        [&](std::size_t step, const egraph::WorkerEvent& event) {
            ran.events.push_back(
                std::format("{} {}", requests.name(step), egraph::describe_event(event)));
        },
        [&](std::size_t step, egraph::Traced what) {
            constexpr std::array names{"build_started", "built",  "build_failed",
                                       "merge_started", "merged", "merge_failed"};
            ran.trace.push_back(std::format("{} {}", names.at(static_cast<std::size_t>(what)),
                                            requests.name(step)));
        });
    ran.done = requests.done_steps;
    CHECK(schedule.finished());
    return ran;
}

} // namespace

TEST_CASE(
    "a pool builds beside each other up to the jobs, each merge on the worker that built it") {
    FakePool pool;
    const auto ran = run(planned({{}, {}, {}}), 2, pool);
    CHECK(ran.outcome.done == 3);
    CHECK_FALSE(ran.outcome.stopped);
    CHECK(ran.trace == std::vector<std::string>{"build_started a", "build_started b", "built a",
                                                "build_started c", "built b", "built c",
                                                "merge_started a", "merged a", "merge_started b",
                                                "merged b", "merge_started c", "merged c"});
    CHECK(pool.sent == std::vector<std::vector<std::string>>{
                           {"build a", "build c", "merge a", "merge c"}, {"build b", "merge b"}});
    CHECK(ran.done == std::vector<std::size_t>{0, 1, 2});
    CHECK(ran.events.front() == "a compile");
    CHECK(ran.events.back() == "c merged");
}

TEST_CASE("each build takes a jobserver's token, given back as it ends") {
    FakePool pool;
    pool.tokens = 1;
    const auto ran = run(planned({{}, {}}), std::nullopt, pool);
    CHECK(ran.outcome.done == 2);
    CHECK(ran.trace == std::vector<std::string>{"build_started a", "built a", "merge_started a",
                                                "merged a", "build_started b", "built b",
                                                "merge_started b", "merged b"});
    CHECK(pool.tokens == 1);
    CHECK(pool.sent.size() == 1);
}

TEST_CASE("a token another holder gives back lets the run go on") {
    FakePool pool;
    pool.tokens = 0;
    pool.freed_later = 1;
    const auto ran = run(planned({{}}), std::nullopt, pool);
    CHECK(ran.outcome.done == 1);
    CHECK(pool.tokens == 1);
}

TEST_CASE("after a failed build what runs finishes and what built merges, nothing more") {
    FakePool pool;
    pool.failing = {"build a"};
    const auto ran = run(planned({{}, {}, {}}), 2, pool);
    CHECK(ran.outcome.done == 1);
    REQUIRE(ran.outcome.stopped);
    CHECK(ran.outcome.stopped->step == 0);
    CHECK(ran.outcome.stopped->why == "compile failed with status 1 (log: /l)");
    CHECK(ran.trace == std::vector<std::string>{"build_started a", "build_started b",
                                                "build_failed a", "built b", "merge_started b",
                                                "merged b"});
}

TEST_CASE("a worker that ends fails its request and what it built") {
    FakePool pool;
    pool.ending = {"build c"};
    const auto ran = run(planned({{}, {}, {}}), 2, pool);
    REQUIRE(ran.outcome.stopped);
    CHECK(ran.outcome.stopped->step == 2);
    CHECK(ran.outcome.stopped->why == "the worker ended before the request was done");
    CHECK(ran.outcome.done == 1);
    CHECK(ran.trace == std::vector<std::string>{"build_started a", "build_started b", "built a",
                                                "build_started c", "built b", "build_failed c",
                                                "merge_failed a", "merge_started b", "merged b"});
}

TEST_CASE("an uninstall runs on an idle worker, beside a build") {
    auto planned_run = planned({{}, {}});
    planned_run.plan.uninstalls.push_back({.package = 0, .why = {}, .after = {0}});
    planned_run.steps.insert(planned_run.steps.begin() + 1,
                             egraph::UninstallStep{.uninstall = 0, .clean_world = false});
    FakePool pool;
    const auto ran = run(planned_run, 1, pool);
    CHECK(ran.outcome.done == 3);
    CHECK(ran.trace == std::vector<std::string>{"build_started a", "built a", "merge_started a",
                                                "merged a", "merge_started b", "build_started c",
                                                "merged b", "built c", "merge_started c",
                                                "merged c"});
    CHECK(pool.sent == std::vector<std::vector<std::string>>{{"build a", "merge a", "uninstall b"},
                                                             {"build c", "merge c"}});
}

TEST_CASE("a refused request, a worker that takes no more or talks nonsense, fails its step") {
    const auto why = [](auto configure) {
        FakePool pool;
        configure(pool);
        const auto ran = run(planned({{}}), 1, pool);
        CHECK(ran.outcome.done == 0);
        REQUIRE(ran.outcome.stopped);
        CHECK(ran.outcome.stopped->step == 0);
        return ran.outcome.stopped->why;
    };
    CHECK(why([](FakePool& pool) { pool.refused = {"build a"}; }) == "no ebuild");
    CHECK(why([](FakePool& pool) { pool.deaf = {"merge a"}; }) ==
          "the worker takes no more requests");
    CHECK(why([](FakePool& pool) {
              pool.garbled = {"build a"};
          }).starts_with("the worker reported what egraph cannot read: "));
}

TEST_CASE("without room for another job, builds run one at a time") {
    FakePool pool;
    pool.room = false;
    const auto ran = run(planned({{}, {}}), 3, pool);
    CHECK(ran.outcome.done == 2);
    CHECK(ran.trace == std::vector<std::string>{"build_started a", "built a", "merge_started a",
                                                "merged a", "build_started b", "built b",
                                                "merge_started b", "merged b"});
    // Asked only beside a running build.
    CHECK_FALSE(pool.asked_room.empty());
    CHECK(std::ranges::all_of(pool.asked_room, [](std::uint32_t running) { return running == 1; }));
}
