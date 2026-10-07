#include "notify.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <ranges>
#include <utility>

namespace egraph {

using Json = nlohmann::json;

std::string notified_json(std::span<const Notified> notified) {
    auto entries = Json::array();
    for (const auto& entry : notified) {
        entries.push_back({{"key", entry.key},
                           {"fingerprint", entry.fingerprint},
                           {"at", entry.at.time_since_epoch().count()}});
    }
    const Json document{{"format", 1}, {"notified", std::move(entries)}};
    return document.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
}

std::expected<std::vector<Notified>, std::string> parse_notified(std::string_view text) {
    const auto document = Json::parse(text, nullptr, false);
    const auto invalid = std::unexpected(std::string{"not a notified file"});
    if (!document.is_object()) {
        return invalid;
    }
    const auto format = document.find("format");
    const auto entries = document.find("notified");
    if (format == document.end() || !format->is_number_unsigned() || entries == document.end() ||
        !entries->is_array()) {
        return invalid;
    }
    if (const auto version = format->get<std::uint64_t>(); version != 1) {
        return std::unexpected(std::format("format {}, from another egraph version", version));
    }
    std::vector<Notified> read;
    for (const auto& entry : *entries) {
        const auto key = entry.find("key");
        const auto fingerprint = entry.find("fingerprint");
        const auto at = entry.find("at");
        if (!entry.is_object() || key == entry.end() || !key->is_string() ||
            fingerprint == entry.end() || !fingerprint->is_string() || at == entry.end() ||
            !at->is_number_integer()) {
            return invalid;
        }
        read.push_back({.key = key->get<std::string>(),
                        .fingerprint = fingerprint->get<std::string>(),
                        .at = Seconds{std::chrono::seconds{at->get<std::int64_t>()}}});
    }
    return read;
}

std::optional<std::filesystem::path> notified_path(const std::optional<std::string>& state_home,
                                                   const std::optional<std::string>& home) {
    return set_aside_path(state_home, home).transform([](const std::filesystem::path& path) {
        return path.parent_path() / "notified.json";
    });
}

std::vector<Notice> to_notify(std::span<const Notice> shown, std::span<const Notified> notified,
                              std::span<const SetAside> set_aside, Seconds now) {
    std::vector<Notice> fresh;
    for (const auto& notice : shown) {
        const auto carried = std::ranges::find(notified, notice.key, &Notified::key);
        const auto aside = std::ranges::find(set_aside, notice.key, &SetAside::key);
        const bool same = carried != notified.end() && carried->fingerprint == notice.fingerprint;
        const bool came_back = same && aside != set_aside.end() && aside->until &&
                               *aside->until <= now && *aside->until > carried->at;
        if (!same || came_back) {
            fresh.push_back(notice);
        }
    }
    return fresh;
}

std::vector<Notified> notified_now(std::span<const Notice> shown, Seconds now) {
    std::vector<Notified> notified;
    notified.reserve(shown.size());
    for (const auto& notice : shown) {
        notified.push_back({.key = notice.key, .fingerprint = notice.fingerprint, .at = now});
    }
    return notified;
}

std::optional<Seconds> next_due(std::span<const SetAside> set_aside, Seconds now) {
    std::optional<Seconds> next;
    for (const auto& entry : set_aside) {
        if (entry.until && *entry.until > now && (!next || *entry.until < *next)) {
            next = entry.until;
        }
    }
    return next;
}

Summary summary(std::span<const Notice> shown, std::span<const Notice> fresh) {
    if (shown.size() == 1) {
        const auto& only = shown.front();
        std::vector<std::string> body;
        std::ranges::copy(only.detail | std::views::take(summary_lines), std::back_inserter(body));
        return {.title = only.title, .body = std::move(body)};
    }
    const auto is_fresh = [&fresh](const Notice& notice) {
        return std::ranges::contains(fresh, notice.key, &Notice::key);
    };
    std::vector<std::string> titles;
    for (const auto& notice : shown | std::views::filter(is_fresh)) {
        titles.push_back(notice.title);
    }
    for (const auto& notice : shown | std::views::filter(std::not_fn(is_fresh))) {
        titles.push_back(notice.title);
    }
    const auto count = static_cast<std::size_t>(std::ranges::count_if(shown, is_fresh));
    Summary out{.title = count == shown.size()
                             ? std::format("{} notices", shown.size())
                             : std::format("{} notices, {} new", shown.size(), count),
                .body = {}};
    if (titles.size() <= summary_lines + 1) {
        out.body = std::move(titles);
    } else {
        std::ranges::move(titles | std::views::take(summary_lines), std::back_inserter(out.body));
        out.body.push_back(std::format("and {} more", titles.size() - summary_lines));
    }
    return out;
}

namespace {

// How a known terminal is told the command to run: after these.
std::vector<std::string> taking_command(std::string program) {
    const auto name = std::filesystem::path{program}.filename().string();
    if (name == "xdg-terminal-exec" || name == "foot" || name == "kitty") {
        return {std::move(program)};
    }
    if (name == "gnome-terminal") {
        return {std::move(program), "--"};
    }
    return {std::move(program), "-e"};
}

} // namespace

std::optional<std::vector<std::string>>
terminal_command(std::string_view setting, const std::optional<std::string>& terminal_variable,
                 std::span<const std::string_view> installed) {
    std::vector<std::string> words;
    for (const auto word : std::views::split(setting, ' ')) {
        if (!word.empty()) {
            words.emplace_back(word.begin(), word.end());
        }
    }
    if (!words.empty()) {
        return words;
    }
    if (std::ranges::contains(installed, std::string_view{"xdg-terminal-exec"})) {
        return taking_command("xdg-terminal-exec");
    }
    if (terminal_variable && !terminal_variable->empty()) {
        return taking_command(*terminal_variable);
    }
    for (const auto name : terminals) {
        if (std::ranges::contains(installed, name)) {
            return taking_command(std::string{name});
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> find_program(std::string_view name, std::string_view path) {
    for (const auto entry : std::views::split(path, ':')) {
        const std::filesystem::path directory{std::string_view{entry.begin(), entry.end()}};
        if (!directory.is_absolute()) {
            continue;
        }
        auto candidate = directory / name;
        std::error_code error;
        const auto status = std::filesystem::status(candidate, error);
        constexpr auto executable = std::filesystem::perms::owner_exec |
                                    std::filesystem::perms::group_exec |
                                    std::filesystem::perms::others_exec;
        if (!error && std::filesystem::is_regular_file(status) &&
            (status.permissions() & executable) != std::filesystem::perms::none) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<SummaryAction> summary_action(std::string_view key) {
    if (key == "default" || key == "open") {
        return SummaryAction::open;
    }
    if (key == "later") {
        return SummaryAction::later;
    }
    if (key == "dismiss") {
        return SummaryAction::dismiss;
    }
    return std::nullopt;
}

bus::Notification summary_notification(const Summary& summary, std::uint32_t replaces) {
    std::string body;
    for (const auto& line : summary.body) {
        if (!body.empty()) {
            body += '\n';
        }
        body += line;
    }
    return {.replaces = replaces,
            .summary = summary.title,
            .body = std::move(body),
            .actions = {{"default", "Open"},
                        {"open", "Open"},
                        {"later", "Later"},
                        {"dismiss", "Dismiss"}},
            .expire = -1};
}

} // namespace egraph
