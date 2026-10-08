#include "helpers.hpp"

#include "notify.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using egraph::Notice;
using egraph::NoticeKind;
using egraph::Notified;
using egraph::SetAside;
using Strings = std::vector<std::string>;
using namespace std::chrono_literals;

const egraph::Seconds now{std::chrono::sys_days{std::chrono::October / 7 / 2026}};

Notice notice(std::string key, std::string fingerprint, std::string title = "") {
    return {.kind = NoticeKind::glsa,
            .key = std::move(key),
            .title = std::move(title),
            .fingerprint = std::move(fingerprint)};
}

Strings keys(const std::vector<Notice>& notices) {
    Strings out;
    for (const auto& each : notices) {
        out.push_back(each.key);
    }
    return out;
}

} // namespace

TEST_CASE("what a summary carried is remembered as JSON") {
    const std::vector<Notified> notified{{.key = "glsa:1", .fingerprint = "a", .at = now},
                                         {.key = "config", .fingerprint = "/etc/x", .at = now}};
    const auto read = egraph::parse_notified(egraph::notified_json(notified));
    REQUIRE(read.has_value());
    CHECK(*read == notified);
    CHECK(egraph::parse_notified(egraph::notified_json({}))->empty());
    CHECK_FALSE(egraph::parse_notified("[]").has_value());
    CHECK_FALSE(egraph::parse_notified(R"({"format":1,"notified":[{"key":"x"}]})").has_value());
    CHECK(egraph::parse_notified(R"({"format":2,"notified":[]})").error() ==
          "format 2, from another egraph version");
}

TEST_CASE("the notified file is in the user's state directory") {
    CHECK(egraph::notified_path("/state", "/home/u") == "/state/egraph/notified.json");
    CHECK(egraph::notified_path(std::nullopt, "/home/u") ==
          "/home/u/.local/state/egraph/notified.json");
    // A relative XDG_STATE_HOME is ignored, as the spec says.
    CHECK(egraph::notified_path("state", "/home/u") == "/home/u/.local/state/egraph/notified.json");
    CHECK_FALSE(egraph::notified_path(std::nullopt, std::nullopt).has_value());
}

TEST_CASE("a summary is called for by a notice the last one did not carry as it is") {
    const std::vector<Notice> shown{notice("glsa:1", "a"), notice("glsa:2", "b"),
                                    notice("glsa:3", "c")};
    const std::vector<Notified> notified{{.key = "glsa:1", .fingerprint = "a", .at = now - 1h},
                                         // Revised since.
                                         {.key = "glsa:2", .fingerprint = "old", .at = now - 1h},
                                         {.key = "glsa:9", .fingerprint = "z", .at = now - 1h}};
    CHECK(keys(egraph::to_notify(shown, notified, {}, now)) == Strings{"glsa:2", "glsa:3"});
    CHECK(keys(egraph::to_notify(shown, {}, {}, now)) == Strings{"glsa:1", "glsa:2", "glsa:3"});
    CHECK(egraph::to_notify(shown, egraph::notified_now(shown, now), {}, now).empty());
    CHECK(egraph::to_notify({}, notified, {}, now).empty());
}

TEST_CASE("a notice put off calls for a summary again once its time comes") {
    const std::vector<Notice> shown{notice("glsa:1", "a"), notice("glsa:2", "b")};
    const std::vector<Notified> notified{{.key = "glsa:1", .fingerprint = "a", .at = now - 2h},
                                         {.key = "glsa:2", .fingerprint = "b", .at = now - 2h}};
    // Put off for an hour, an hour ago: back now.
    const std::vector<SetAside> due{{.key = "glsa:1", .fingerprint = "a", .until = now - 1h}};
    CHECK(keys(egraph::to_notify(shown, notified, due, now)) == Strings{"glsa:1"});
    // Already carried again since it came back.
    const std::vector<Notified> since{{.key = "glsa:1", .fingerprint = "a", .at = now - 30min},
                                      {.key = "glsa:2", .fingerprint = "b", .at = now - 30min}};
    CHECK(egraph::to_notify(shown, since, due, now).empty());
    // A dismissal never comes due.
    const std::vector<SetAside> dismissed{{.key = "glsa:1", .fingerprint = "a"}};
    CHECK(egraph::to_notify(shown, notified, dismissed, now).empty());
}

TEST_CASE("a summary remembers each notice it carries as of now") {
    const std::vector<Notice> shown{notice("glsa:1", "a"), notice("config", "/etc/._cfg0000_x")};
    CHECK(egraph::notified_now(shown, now) ==
          std::vector<Notified>{{.key = "glsa:1", .fingerprint = "a", .at = now},
                                {.key = "config", .fingerprint = "/etc/._cfg0000_x", .at = now}});
}

TEST_CASE("the next thing put off comes back at its time") {
    const std::vector<SetAside> set_aside{{.key = "a", .fingerprint = "1", .until = now + 3h},
                                          {.key = "b", .fingerprint = "1"},
                                          {.key = "c", .fingerprint = "1", .until = now + 1h},
                                          {.key = "d", .fingerprint = "1", .until = now - 1h}};
    CHECK(egraph::next_due(set_aside, now) == now + 1h);
    CHECK_FALSE(egraph::next_due({}, now).has_value());
    CHECK_FALSE(egraph::next_due(std::span{set_aside}.subspan(1, 1), now).has_value());
}

TEST_CASE("a summary of one notice is its title and detail") {
    auto glsa = notice("glsa:1", "a", "GLSA 202609-03: libfoo: heap overflow");
    glsa.detail = {"affects dev-libs/b-1, fixed in >=dev-libs/b-2"};
    const std::vector<Notice> shown{glsa};
    CHECK(egraph::summary(shown, shown) ==
          egraph::Summary{.title = "GLSA 202609-03: libfoo: heap overflow",
                          .body = {"affects dev-libs/b-1, fixed in >=dev-libs/b-2"}});
}

TEST_CASE("a summary of several counts them, the fresh ones first") {
    std::vector<Notice> shown;
    for (int at = 1; at <= 7; ++at) {
        shown.push_back(notice(std::format("glsa:{}", at), "a", std::format("notice {}", at)));
    }
    const std::vector<Notice> fresh{shown.at(5), shown.at(6)};
    CHECK(egraph::summary(shown, fresh) ==
          egraph::Summary{
              .title = "7 notices, 2 new",
              .body = {"notice 6", "notice 7", "notice 1", "notice 2", "notice 3", "and 2 more"}});
    const std::vector<Notice> two{shown.at(0), shown.at(1)};
    CHECK(egraph::summary(two, two) ==
          egraph::Summary{.title = "2 notices", .body = {"notice 1", "notice 2"}});
}

TEST_CASE("Open's terminal: the setting, xdg-terminal-exec, $TERMINAL, then the first found") {
    using egraph::terminal_command;
    using Installed = std::vector<std::string_view>;
    const Installed all{"xdg-terminal-exec", "foot", "xterm"};
    CHECK(terminal_command("  wezterm  start -- ", "kitty", all) ==
          Strings{"wezterm", "start", "--"});
    CHECK(terminal_command("", "kitty", all) == Strings{"xdg-terminal-exec"});
    // $TERMINAL as each known one takes a command; anything else with -e, as xterm.
    CHECK(terminal_command("", "kitty", Installed{"foot"}) == Strings{"kitty"});
    CHECK(terminal_command("", "/usr/bin/gnome-terminal", {}) ==
          Strings{"/usr/bin/gnome-terminal", "--"});
    CHECK(terminal_command("", "urxvt", {}) == Strings{"urxvt", "-e"});
    CHECK(terminal_command("", "", Installed{"foot", "xterm"}) == Strings{"foot"});
    CHECK(terminal_command("", std::nullopt, Installed{"alacritty"}) == Strings{"alacritty", "-e"});
    CHECK(terminal_command("", std::nullopt, Installed{"konsole"}) == Strings{"konsole", "-e"});
    CHECK(terminal_command("", std::nullopt, Installed{"xterm"}) == Strings{"xterm", "-e"});
    CHECK_FALSE(terminal_command("", std::nullopt, {}).has_value());
}

TEST_CASE("a program is found in PATH where it is executable") {
    const egraph::test::TempDir root;
    std::filesystem::create_directories(root.path() / "a");
    std::filesystem::create_directories(root.path() / "b");
    egraph::test::write_text(root.path() / "a/foot", "");
    egraph::test::write_text(root.path() / "b/foot", "");
    std::filesystem::permissions(root.path() / "b/foot", std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    std::filesystem::create_directories(root.path() / "b/xterm");
    const auto path = std::format("{}:{}:{}", (root.path() / "missing").string(),
                                  (root.path() / "a").string(), (root.path() / "b").string());
    CHECK(egraph::find_program("foot", path) == root.path() / "b/foot");
    // A directory is not a program.
    CHECK_FALSE(egraph::find_program("xterm", path).has_value());
    CHECK_FALSE(egraph::find_program("foot", "").has_value());
    // Relative entries are skipped.
    CHECK_FALSE(egraph::find_program("foot", "b").has_value());
}

TEST_CASE("a summary's buttons: Open, also a click on it, Later and Dismiss") {
    using egraph::SummaryAction;
    CHECK(egraph::summary_action("default") == SummaryAction::open);
    CHECK(egraph::summary_action("open") == SummaryAction::open);
    CHECK(egraph::summary_action("later") == SummaryAction::later);
    CHECK(egraph::summary_action("dismiss") == SummaryAction::dismiss);
    CHECK_FALSE(egraph::summary_action("other"));
    const auto posted =
        egraph::summary_notification({.title = "2 notices", .body = {"one", "two"}}, 4);
    CHECK(posted.replaces == 4);
    CHECK(posted.summary == "2 notices");
    CHECK(posted.body == "one\ntwo");
    using Actions = std::vector<std::pair<std::string, std::string>>;
    CHECK(
        posted.actions ==
        Actions{{"default", "Open"}, {"open", "Open"}, {"later", "Later"}, {"dismiss", "Dismiss"}});
    CHECK(posted.expire == -1);
}

namespace {

using egraph::NotifyWake;
using egraph::SummaryAction;

// The notices, what is set aside and what was notified, and the bus, as the loop sees them; each
// wait runs the next step on it, a stop once there are none.
const egraph::Seconds start = now;

struct FakeWorld {
    egraph::Seconds time = start;
    std::vector<Notice> all{};
    std::vector<SetAside> set_aside{};
    std::vector<Notified> notified{};
    std::uint32_t next_id = 1;
    bool post_fails = false;
    bool close_fails = false;
    std::vector<egraph::bus::Notification> posts{};
    std::vector<std::uint32_t> closes{};
    std::vector<std::pair<SummaryAction, Strings>> acts{};
    std::vector<std::optional<std::chrono::milliseconds>> timeouts{};
    std::vector<std::function<std::expected<NotifyWake, std::string>(FakeWorld&)>> steps{};
    std::size_t step = 0;
    std::size_t restarts = 0;
    // Why a restart fails; none succeeds.
    std::optional<std::string> restart_error{};

    [[nodiscard]] egraph::Seconds now() const { return time; }
    [[nodiscard]] egraph::NotifyView view() const {
        return {.shown = egraph::shown_notices(all, set_aside, time),
                .set_aside = set_aside,
                .notified = notified};
    }
    std::expected<std::uint32_t, std::string> post(const egraph::bus::Notification& posted) {
        if (post_fails) {
            return std::unexpected(std::string{"no server"});
        }
        posts.push_back(posted);
        return posted.replaces != 0 ? posted.replaces : next_id++;
    }
    std::expected<void, std::string> close(std::uint32_t id) {
        closes.push_back(id);
        if (close_fails) {
            return std::unexpected(std::format("no notification {}", id));
        }
        return {};
    }
    std::expected<void, std::string> remember(std::span<const Notified> carried) {
        notified.assign(carried.begin(), carried.end());
        return {};
    }
    std::expected<void, std::string> act(SummaryAction action, std::span<const Notice> notices) {
        acts.emplace_back(action, keys({notices.begin(), notices.end()}));
        if (action != SummaryAction::open) {
            for (const auto& notice : notices) {
                egraph::set_notice_aside(set_aside, notice,
                                         action == SummaryAction::later
                                             ? std::optional{time + egraph::summary_later}
                                             : std::nullopt,
                                         all);
            }
        }
        return {};
    }
    std::string restart() {
        ++restarts;
        return restart_error.value_or("");
    }
    std::expected<NotifyWake, std::string> wait(std::optional<std::chrono::milliseconds> timeout) {
        timeouts.push_back(timeout);
        if (step == steps.size()) {
            return NotifyWake{.stop = true};
        }
        return steps.at(step++)(*this);
    }
};

NotifyWake nothing() {
    return {};
}

NotifyWake clicked(std::uint32_t id, std::string key) {
    return {
        .stop = false,
        .events = {{.kind = egraph::bus::Event::Kind::action, .id = id, .action = std::move(key)}}};
}

} // namespace

TEST_CASE("notify posts a summary of new notices once, and closes it when stopped") {
    FakeWorld world{.all = {notice("glsa:1", "a", "one"), notice("glsa:2", "b", "two")}};
    world.steps = {[](FakeWorld&) { return nothing(); }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    REQUIRE(world.posts.size() == 1);
    CHECK(world.posts.front().replaces == 0);
    CHECK(world.posts.front().summary == "2 notices");
    CHECK(world.notified == egraph::notified_now(world.all, now));
    CHECK(world.closes == std::vector<std::uint32_t>{1});
    CHECK(world.timeouts ==
          std::vector<std::optional<std::chrono::milliseconds>>{std::nullopt, std::nullopt});
    CHECK(log.str().empty());
}

TEST_CASE("notify leaves alone the notices the last summary carried") {
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.notified = egraph::notified_now(world.all, now - 1h);
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.posts.empty());
    CHECK(world.closes.empty());
}

TEST_CASE("notify replaces the summary in place for a new notice") {
    FakeWorld world{.all = {notice("glsa:1", "a", "one")}};
    world.steps = {[](FakeWorld& w) {
        w.all.push_back(notice("glsa:2", "b", "two"));
        return nothing();
    }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    REQUIRE(world.posts.size() == 2);
    CHECK(world.posts.at(1).replaces == 1);
    CHECK(world.posts.at(1).summary == "2 notices, 1 new");
    CHECK(world.posts.at(1).body == "two\none");
}

TEST_CASE("notify closes the summary once nothing is left, and posts anew after") {
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.steps = {[](FakeWorld& w) {
                       w.all.clear();
                       return nothing();
                   },
                   [](FakeWorld& w) {
                       w.all.push_back(notice("glsa:2", "b"));
                       return nothing();
                   }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.closes == std::vector<std::uint32_t>{1, 2});
    REQUIRE(world.posts.size() == 2);
    CHECK(world.posts.at(1).replaces == 0);
}

TEST_CASE("Dismiss sets aside what the summary carried, not what came since") {
    FakeWorld world{.all = {notice("glsa:1", "a"), notice("glsa:2", "b")}};
    world.steps = {[](FakeWorld& w) {
        w.all.push_back(notice("glsa:3", "c", "three"));
        return clicked(1, "dismiss");
    }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.acts == std::vector<std::pair<SummaryAction, Strings>>{
                            {SummaryAction::dismiss, {"glsa:1", "glsa:2"}}});
    REQUIRE(world.posts.size() == 2);
    // The dismissed summary is gone; the new notice has one of its own.
    CHECK(world.posts.at(1).replaces == 0);
    CHECK(world.posts.at(1).summary == "three");
    CHECK(world.closes == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("Later puts the summary's notices off a day, and the summary comes back then") {
    FakeWorld world{.all = {notice("glsa:1", "a", "one")}};
    world.steps = {[](FakeWorld&) { return clicked(1, "later"); },
                   [](FakeWorld& w) {
                       w.time += egraph::summary_later;
                       return nothing();
                   }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.acts ==
          std::vector<std::pair<SummaryAction, Strings>>{{SummaryAction::later, {"glsa:1"}}});
    CHECK(world.timeouts.at(1) == std::chrono::milliseconds{egraph::summary_later});
    REQUIRE(world.posts.size() == 2);
    CHECK(world.posts.at(1).summary == "one");
    CHECK(world.posts.at(1).replaces == 0);
}

TEST_CASE("Open, or a click on the summary, opens the notices and sets nothing aside") {
    const auto key = GENERATE(std::string{"open"}, std::string{"default"});
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.steps = {[key](FakeWorld&) { return clicked(1, key); }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.acts ==
          std::vector<std::pair<SummaryAction, Strings>>{{SummaryAction::open, {"glsa:1"}}});
    CHECK(world.set_aside.empty());
    CHECK(world.posts.size() == 1);
    CHECK(world.closes == std::vector<std::uint32_t>{1});
}

TEST_CASE("an action on a summary the server closed itself logs nothing") {
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.steps = {[](FakeWorld& w) {
        w.close_fails = true;
        return clicked(1, "later");
    }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.closes == std::vector<std::uint32_t>{1});
    CHECK(log.str().empty());
}

TEST_CASE("notify ignores other notifications and unknown actions") {
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.steps = {[](FakeWorld&) { return clicked(9, "dismiss"); },
                   [](FakeWorld&) { return clicked(1, "other"); }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.acts.empty());
    CHECK(world.closes == std::vector<std::uint32_t>{1});
}

TEST_CASE("a summary the server closed is posted anew for the next notice") {
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.steps = {[](FakeWorld& w) {
        w.all.push_back(notice("glsa:2", "b"));
        return NotifyWake{.stop = false,
                          .events = {{.kind = egraph::bus::Event::Kind::closed, .id = 1}}};
    }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    REQUIRE(world.posts.size() == 2);
    CHECK(world.posts.at(1).replaces == 0);
    // Only the summary still up is closed on the way out.
    CHECK(world.closes == std::vector<std::uint32_t>{2});
}

TEST_CASE("a summary that cannot be posted is logged and tried again on the next wake") {
    FakeWorld world{.all = {notice("glsa:1", "a")}, .post_fails = true};
    world.steps = {[](FakeWorld& w) {
        w.post_fails = false;
        return nothing();
    }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(log.str() == "egraph: notify: no server\n");
    CHECK(world.posts.size() == 1);
}

TEST_CASE("notify ends with the error its wait ended with") {
    FakeWorld world;
    world.steps = {[](FakeWorld&) -> std::expected<NotifyWake, std::string> {
        return std::unexpected(std::string{"the session bus failed: gone"});
    }};
    std::ostringstream log;
    const auto kept = egraph::keep_notified(world, log);
    REQUIRE_FALSE(kept);
    CHECK(kept.error() == "the session bus failed: gone");
}

TEST_CASE("a replaced executable takes the summary down and forgets it, for the new one to post") {
    FakeWorld world{.all = {notice("glsa:1", "a")}};
    world.steps = {[](FakeWorld&) { return NotifyWake{.replaced = true}; }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.restarts == 1);
    CHECK(world.closes == std::vector<std::uint32_t>{1});
    CHECK(world.notified.empty());
    CHECK(log.str() == "egraph: notify: restarting, as its executable was replaced\n");
}

TEST_CASE("a restart that fails is logged, and the summary posted again") {
    FakeWorld world{.all = {notice("glsa:1", "a")}, .restart_error = "egraph: Permission denied"};
    world.steps = {[](FakeWorld&) { return NotifyWake{.replaced = true}; }};
    std::ostringstream log;
    REQUIRE(egraph::keep_notified(world, log));
    CHECK(world.restarts == 1);
    CHECK(world.posts.size() == 2);
    CHECK(world.notified == egraph::notified_now(world.all, now));
    CHECK(log.str().ends_with(
        "egraph: notify: cannot restart, so keeps running: egraph: Permission denied\n"));
}
