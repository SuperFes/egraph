#pragma once

// What egraph notify tells the desktop: which notices call for a summary notification, what it
// says, and the terminal its Open starts the interface in.

#include "notices.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

} // namespace egraph
