#pragma once

// What egraph notify tells the desktop: which notices call for a summary notification, what it
// says, and the terminal its Open starts the interface in.

#include "bus.hpp"
#include "notices.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace egraph {

// A notice as the last summary carried it.
struct Notified {
    std::string key;
    std::string fingerprint;
    Seconds at{};
    auto operator<=>(const Notified&) const = default;
};

[[nodiscard]] std::string notified_json(std::span<const Notified> notified);
[[nodiscard]] std::expected<std::vector<Notified>, std::string>
parse_notified(std::string_view text);

// ${XDG_STATE_HOME:-$HOME/.local/state}/egraph/notified.json; none without either.
[[nodiscard]] std::optional<std::filesystem::path>
notified_path(const std::optional<std::string>& state_home, const std::optional<std::string>& home);

// The shown notices calling for a summary: those the last summary did not carry as they are now,
// and those put off whose time came after it carried them.
[[nodiscard]] std::vector<Notice> to_notify(std::span<const Notice> shown,
                                            std::span<const Notified> notified,
                                            std::span<const SetAside> set_aside, Seconds now);

// What a summary of the shown notices carries, to remember.
[[nodiscard]] std::vector<Notified> notified_now(std::span<const Notice> shown, Seconds now);

// The earliest time something put off comes back after now.
[[nodiscard]] std::optional<Seconds> next_due(std::span<const SetAside> set_aside, Seconds now);

struct Summary {
    std::string title;
    std::vector<std::string> body;
    bool operator==(const Summary&) const = default;
};

// A single notice as its title and detail; several counted, with a line each, the fresh ones
// first, at most summary_lines of them.
inline constexpr std::size_t summary_lines = 5;
[[nodiscard]] Summary summary(std::span<const Notice> shown, std::span<const Notice> fresh);

// The terminals Open looks for in PATH, in order.
inline constexpr std::array<std::string_view, 7> terminals{
    "xdg-terminal-exec", "foot", "alacritty", "kitty", "konsole", "gnome-terminal", "xterm"};

// The command line a terminal runs a program with, before the program's own: the settings
// file's terminal as written, else xdg-terminal-exec, else $TERMINAL, else the first of the rest
// of terminals installed (each as it takes a command); none without any. installed names those
// of terminals found in PATH.
[[nodiscard]] std::optional<std::vector<std::string>>
terminal_command(std::string_view setting, const std::optional<std::string>& terminal_variable,
                 std::span<const std::string_view> installed);

// The executable name is found as in PATH (a colon-separated list); none where it is not.
[[nodiscard]] std::optional<std::filesystem::path> find_program(std::string_view name,
                                                                std::string_view path);

// The bus name a running egraph notify holds, so that a second one in the session stands down.
inline constexpr std::string_view notify_bus_name = "io.github.SuperFes.egraph.Notify";

// What a summary's buttons do to the notices it carries.
enum class SummaryAction : std::uint8_t { open, later, dismiss };

// The action a key from the server means; a click on the summary itself opens.
[[nodiscard]] std::optional<SummaryAction> summary_action(std::string_view key);

// How long Later puts the notices off.
inline constexpr std::chrono::seconds summary_later{std::chrono::days{1}};

// The notification a summary posts, in place of the one replaces names (0 for none).
[[nodiscard]] bus::Notification summary_notification(const Summary& summary,
                                                     std::uint32_t replaces);

// What the notify loop sees when it wakes.
struct NotifyView {
    // The notices not set aside, as of now.
    std::vector<Notice> shown;
    std::vector<SetAside> set_aside;
    // What the last summary carried.
    std::vector<Notified> notified;
};

// What a wait of the notify loop's ended with: a stop asked, what happened on the bus, or the
// executable replaced.
struct NotifyWake {
    bool stop = false;
    std::vector<bus::Event> events{};
    bool replaced = false;
};

// Keeps one summary notification of the notices up until a stop is asked: posted for notices new
// since the last summary or put off until now, in place of the one still up; closed when none is
// left; its actions done on the notices it carries. The world gives:
//   now() -> Seconds
//   view() -> NotifyView, as it is now
//   post(const bus::Notification&) -> std::expected<std::uint32_t, std::string>, the id
//   close(std::uint32_t) -> std::expected<void, std::string>
//   remember(std::span<const Notified>) -> std::expected<void, std::string>
//   act(SummaryAction, std::span<const Notice>) -> std::expected<void, std::string>
//   wait(std::optional<std::chrono::milliseconds>) -> std::expected<NotifyWake, std::string>,
//     until something may have changed, the time passes, or a stop is asked.
//   restart() -> std::string, why the executable could not be run again over this process
//     (empty only from a fake that did, which ends the loop).
// The summary is taken down and forgotten before a restart, so the new process posts it again.
// What fails is logged and the loop goes on; the error when waiting does.
template <class World>
std::expected<void, std::string> keep_notified(World& world, std::ostream& log) {
    const auto logged = [&log](const auto& done) {
        if (!done) {
            log << "egraph: notify: " << done.error() << '\n';
        }
    };
    // The summary up, and the notices it carries.
    std::optional<std::uint32_t> posted;
    std::vector<Notice> carried;
    const auto take_down = [&] {
        if (posted) {
            logged(world.close(*posted));
            posted.reset();
        }
    };
    while (true) {
        const auto now = world.now();
        const auto view = world.view();
        const auto fresh = to_notify(view.shown, view.notified, view.set_aside, now);
        if (view.shown.empty()) {
            take_down();
            carried.clear();
        } else if (!fresh.empty()) {
            const auto id =
                world.post(summary_notification(summary(view.shown, fresh), posted.value_or(0)));
            logged(id);
            if (id) {
                posted = *id;
                carried = view.shown;
                logged(world.remember(notified_now(view.shown, now)));
            }
        }
        const auto due = next_due(view.set_aside, now);
        const auto woken = world.wait(due.transform([now](Seconds at) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(at - now);
        }));
        if (!woken) {
            return std::unexpected(woken.error());
        }
        if (woken->stop) {
            take_down();
            return {};
        }
        if (woken->replaced) {
            take_down();
            logged(world.remember({}));
            log << "egraph: notify: restarting, as its executable was replaced\n";
            const auto failed = world.restart();
            if (failed.empty()) {
                return {};
            }
            log << "egraph: notify: cannot restart, so keeps running: " << failed << '\n';
            continue;
        }
        for (const auto& event : woken->events) {
            if (!posted || event.id != *posted) {
                continue;
            }
            if (event.kind == bus::Event::Kind::closed) {
                posted.reset();
                continue;
            }
            if (const auto action = summary_action(event.action)) {
                logged(world.act(*action, carried));
                // Most servers close it themselves on an action, so this may find it gone.
                (void)world.close(*posted);
                posted.reset();
            }
        }
    }
}

} // namespace egraph
