#include "helpers.hpp"

#include "notify.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
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
