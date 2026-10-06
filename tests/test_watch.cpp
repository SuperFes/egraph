#include "watch.hpp"

#include "os.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using egraph::Debounce;
using egraph::Input;
using egraph::InputKind;
using egraph::os::Woken;
using std::chrono::milliseconds;

namespace {

using Paths = std::vector<std::filesystem::path>;

// A watcher whose waits follow a script, each moving a clock on: a change or a stop after a
// while, or nothing until the timeout. A stop once the script runs out.
struct Step {
    milliseconds after{0};
    std::expected<Woken, std::error_code> woken = Woken{};
    // Waits until the timeout instead, finding nothing.
    bool times_out = false;
};

struct World {
    Debounce::Clock::time_point clock{};
    std::vector<Step> steps;
    std::size_t next = 0;
    std::vector<std::optional<milliseconds>> timeouts;
    std::vector<Paths> watched;
    std::vector<Debounce::Clock::time_point> refreshed;
    // What each refresh returns, in turn; the last again once they run out.
    std::vector<std::expected<Paths, std::string>> refreshes{Paths{"/var/db/pkg"}};
    // What each stale() returns, in turn; false once they run out.
    std::vector<bool> stale;
    std::size_t stale_asked = 0;
    std::optional<std::string> watch_error;
};

struct FakeWatcher {
    World* world;

    [[nodiscard]] std::expected<Woken, std::error_code> wait(std::optional<milliseconds> timeout) {
        world->timeouts.push_back(timeout);
        if (world->next == world->steps.size()) {
            return Woken{.changed = false, .stop = true};
        }
        const auto step = world->steps.at(world->next++);
        if (step.times_out) {
            REQUIRE(timeout);
            world->clock += timeout.value_or(milliseconds{0});
            return Woken{};
        }
        world->clock += step.after;
        return step.woken;
    }
};

std::expected<void, std::string> run(World& world, std::ostream& log) {
    return egraph::keep_fresh(
        [&world]() -> std::expected<Paths, std::string> {
            world.refreshed.push_back(world.clock);
            const auto at = std::min(world.refreshed.size(), world.refreshes.size()) - 1;
            return world.refreshes.at(at);
        },
        [&world](const Paths& directories) -> std::expected<FakeWatcher, std::string> {
            if (world.watch_error) {
                return std::unexpected(*world.watch_error);
            }
            world.watched.push_back(directories);
            return FakeWatcher{&world};
        },
        [&world] {
            const auto at = world.stale_asked++;
            return at < world.stale.size() && world.stale.at(at);
        },
        [&world] { return world.clock; }, log);
}

Step change(milliseconds after) {
    return {.after = after, .woken = Woken{.changed = true}};
}

Debounce::Clock::time_point at(milliseconds since) {
    return Debounce::Clock::time_point{} + since;
}

} // namespace

TEST_CASE("the directories to watch hold every input, or are the input") {
    const std::vector<Input> installed{
        {.path = "/var/db/pkg", .kind = InputKind::directory},
        {.path = "/var/db/pkg/app-misc", .kind = InputKind::directory},
        {.path = "/etc/portage/make.conf", .kind = InputKind::file},
        {.path = "/etc/portage/package.use", .kind = InputKind::missing}};
    const std::vector<Input> evaluated{
        {.path = "/etc/portage/make.profile", .kind = InputKind::symlink},
        {.path = "/var/db/repos/gentoo/metadata/md5-cache/app-misc", .kind = InputKind::directory},
        {.path = "/etc/portage/make.conf", .kind = InputKind::file}};
    const std::vector<std::span<const Input>> layers{installed, evaluated};
    CHECK(egraph::watch_directories(layers) ==
          Paths{"/etc/portage", "/var/db/pkg", "/var/db/pkg/app-misc",
                "/var/db/repos/gentoo/metadata/md5-cache/app-misc"});
    CHECK(egraph::watch_directories({}).empty());
}

TEST_CASE("a burst of changes is due once quiet, or long after its first change") {
    Debounce debounce;
    CHECK_FALSE(debounce.due(at(milliseconds{0})));
    CHECK_FALSE(debounce.wait(at(milliseconds{0})));
    debounce.changed(at(milliseconds{0}));
    CHECK(debounce.wait(at(milliseconds{0})) == Debounce::quiet);
    CHECK_FALSE(debounce.due(at(Debounce::quiet - milliseconds{1})));
    // Another change puts it off.
    debounce.changed(at(milliseconds{2000}));
    CHECK_FALSE(debounce.due(at(Debounce::quiet)));
    CHECK(debounce.wait(at(milliseconds{4000})) == milliseconds{1000});
    CHECK(debounce.due(at(milliseconds{2000} + Debounce::quiet)));
    debounce.reset();
    CHECK_FALSE(debounce.due(at(milliseconds{10000})));
    // A change every second never goes quiet, so the first change's age decides.
    for (milliseconds t{0}; t < Debounce::longest; t += milliseconds{1000}) {
        debounce.changed(at(t));
        CHECK_FALSE(debounce.due(at(t)));
    }
    CHECK(debounce.due(at(Debounce::longest)));
    CHECK(debounce.wait(at(Debounce::longest - milliseconds{500})) == milliseconds{500});
    // Past due, there is no waiting.
    CHECK(debounce.wait(at(Debounce::longest + milliseconds{500})) == milliseconds{0});
}

TEST_CASE("watching refreshes first, then waits for nothing in particular until stopped") {
    World world;
    world.steps = {{.after = milliseconds{5000}, .woken = Woken{.stop = true}}};
    std::ostringstream log;
    CHECK(run(world, log));
    CHECK(world.refreshed == std::vector{at(milliseconds{0})});
    CHECK(world.watched == std::vector<Paths>{{"/var/db/pkg"}});
    CHECK(world.timeouts == std::vector<std::optional<milliseconds>>{std::nullopt});
}

TEST_CASE("changes refresh once they settle, watching what the refresh left") {
    World world;
    world.refreshes = {Paths{"/var/db/pkg"}, Paths{"/var/db/pkg", "/var/db/pkg/app-misc"}};
    world.steps = {change(milliseconds{10000}),
                   change(milliseconds{1000}),
                   {.times_out = true},
                   {.after = milliseconds{1}, .woken = Woken{.stop = true}}};
    std::ostringstream log;
    CHECK(run(world, log));
    CHECK(world.refreshed == std::vector{at(milliseconds{0}), at(milliseconds{14000})});
    CHECK(world.watched ==
          std::vector<Paths>{{"/var/db/pkg"}, {"/var/db/pkg", "/var/db/pkg/app-misc"}});
    CHECK(world.timeouts == std::vector<std::optional<milliseconds>>{
                                std::nullopt, Debounce::quiet, Debounce::quiet, std::nullopt});
}

TEST_CASE(
    "a change during a refresh, which leaves the stores stale, refreshes again once settled") {
    World world;
    world.stale = {true, false};
    world.steps = {{.times_out = true}, {.after = milliseconds{1}, .woken = Woken{.stop = true}}};
    std::ostringstream log;
    CHECK(run(world, log));
    CHECK(world.refreshed == std::vector{at(milliseconds{0}), at(Debounce::quiet)});
    CHECK(world.timeouts ==
          std::vector<std::optional<milliseconds>>{Debounce::quiet, std::nullopt});
    CHECK(world.watched.size() == 2);
}

TEST_CASE("a refresh that fails is logged and tried again later, or on the next change") {
    World world;
    const std::expected<Paths, std::string> failed =
        std::unexpected(std::string{"egraph-build: exit status 1"});
    world.refreshes = {failed, failed, Paths{"/var/db/pkg"}};
    world.steps = {{.times_out = true},
                   change(milliseconds{1000}),
                   {.times_out = true},
                   {.after = milliseconds{1}, .woken = Woken{.stop = true}}};
    std::ostringstream log;
    CHECK(run(world, log));
    CHECK(world.refreshed ==
          std::vector{at(milliseconds{0}), at(Debounce::longest),
                      at(Debounce::longest + milliseconds{1000} + Debounce::quiet)});
    CHECK(log.str().find("egraph-build: exit status 1") != std::string::npos);
    // Each failure waits twice as long, or for a change to settle.
    CHECK(world.timeouts ==
          std::vector<std::optional<milliseconds>>{Debounce::longest, 2 * Debounce::longest,
                                                   Debounce::quiet, std::nullopt});
    // Nothing to watch until a refresh succeeds, nor anything to ask stale().
    CHECK(world.watched == std::vector<Paths>{{}, {}, {"/var/db/pkg"}});
    CHECK(world.stale_asked == 1);
}

TEST_CASE("watching ends with the error when no watcher can be opened or waited on") {
    World world;
    world.watch_error = "inotify: too many open files";
    std::ostringstream log;
    const auto opened = run(world, log);
    REQUIRE_FALSE(opened);
    CHECK(opened.error() == "inotify: too many open files");

    World waiting;
    waiting.steps = {
        {.after = milliseconds{1},
         .woken = std::unexpected(std::make_error_code(std::errc::bad_file_descriptor))}};
    const auto waited = run(waiting, log);
    REQUIRE_FALSE(waited);
    CHECK(waited.error().find("Bad file descriptor") != std::string::npos);
}
