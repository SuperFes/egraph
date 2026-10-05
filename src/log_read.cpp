#include "log_read.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <ranges>

namespace egraph::log {

namespace {

using Json = nlohmann::json;

// The lines of text, one JSON object each, skipping the others.
std::vector<Json> objects(std::string_view text) {
    std::vector<Json> found;
    for (const auto line : std::views::split(text, '\n')) {
        auto json = Json::parse(std::string_view{line}, nullptr, false);
        if (json.is_object()) {
            found.push_back(std::move(json));
        }
    }
    return found;
}

std::string text_of(const Json& json, std::string_view key) {
    const auto found = json.find(key);
    return found != json.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

template <class Number> std::optional<Number> number_in(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return value;
}

const Field* field_of(const Event& event, std::string_view name) {
    const auto found = std::ranges::find(event.fields, name, &Field::name);
    return found == event.fields.end() ? nullptr : &*found;
}

// A field's number, whether written as one or, from the journal, as text.
template <class Number> std::optional<Number> number(const Event& event, std::string_view name) {
    const auto* field = field_of(event, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    return std::visit(
        [](const auto& value) -> std::optional<Number> {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) {
                return number_in<Number>(value);
            } else {
                return static_cast<Number>(value);
            }
        },
        field->value);
}

std::string text(const Event& event, std::string_view name) {
    const auto* field = field_of(event, name);
    const auto* value = field == nullptr ? nullptr : std::get_if<std::string>(&field->value);
    return value == nullptr ? std::string{} : *value;
}

std::string local_time(double time, const std::chrono::time_zone& zone) {
    const auto seconds =
        std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(std::floor(time))}};
    return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::zoned_time{&zone, seconds});
}

} // namespace

std::vector<Event> file_events(std::string_view text) {
    std::vector<Event> events;
    for (const auto& json : objects(text)) {
        const auto time = json.find("time");
        const auto priority = json.find("priority");
        if (time == json.end() || !time->is_number() || priority == json.end() ||
            !priority->is_number_integer()) {
            continue;
        }
        Event event{.time = time->get<double>(),
                    .run = text_of(json, "run"),
                    .kind = text_of(json, "event"),
                    .message = text_of(json, "message"),
                    .priority = priority->get<int>(),
                    .fields = {}};
        for (const auto& [key, value] : json.items()) {
            if (key == "time" || key == "run" || key == "event" || key == "message" ||
                key == "priority") {
                continue;
            }
            if (value.is_string()) {
                event.fields.push_back({.name = key, .value = value.get<std::string>()});
            } else if (value.is_number_integer()) {
                event.fields.push_back({.name = key, .value = value.get<std::int64_t>()});
            } else if (value.is_number()) {
                event.fields.push_back({.name = key, .value = value.get<double>()});
            }
        }
        events.push_back(std::move(event));
    }
    return events;
}

std::vector<Event> journal_events(std::string_view text) {
    constexpr std::string_view prefix = "EGRAPH_";
    std::vector<Event> events;
    for (const auto& json : objects(text)) {
        const auto run = text_of(json, "EGRAPH_RUN");
        const auto kind = text_of(json, "EGRAPH_EVENT");
        if (text_of(json, "SYSLOG_IDENTIFIER") != "egraph" || run.empty() || kind.empty()) {
            continue;
        }
        Event event{.time =
                        number_in<double>(text_of(json, "__REALTIME_TIMESTAMP")).value_or(0) / 1e6,
                    .run = run,
                    .kind = kind,
                    .message = text_of(json, "MESSAGE"),
                    .priority = number_in<int>(text_of(json, "PRIORITY")).value_or(6),
                    .fields = {}};
        for (const auto& [key, value] : json.items()) {
            if (!key.starts_with(prefix) || key == "EGRAPH_RUN" || key == "EGRAPH_EVENT" ||
                !value.is_string()) {
                continue;
            }
            auto name = key.substr(prefix.size());
            std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            event.fields.push_back({.name = std::move(name), .value = value.get<std::string>()});
        }
        events.push_back(std::move(event));
    }
    return events;
}

std::vector<std::string> journal_command() {
    return {"journalctl", "--no-pager", "--output=json", "--identifier=egraph"};
}

std::vector<RunSummary> summarize(std::span<const Event> events) {
    std::vector<RunSummary> runs;
    std::map<std::string, std::size_t, std::less<>> index;
    for (const auto& event : events) {
        auto [at, added] = index.try_emplace(event.run, runs.size());
        if (added) {
            runs.push_back({.run = event.run, .started = event.time, .status = "unfinished"});
        }
        auto& run = runs.at(at->second);
        if (event.kind == "run") {
            run.started = event.time;
            run.command = text(event, "command");
            run.targets = text(event, "targets");
        } else if (event.kind == "end") {
            run.status = text(event, "status") == "ok" ? "done" : "failed";
            run.merged = number<std::int64_t>(event, "merged").value_or(0);
            run.uninstalled = number<std::int64_t>(event, "uninstalled").value_or(0);
            run.failed = number<std::int64_t>(event, "failed").value_or(0);
            run.skipped = number<std::int64_t>(event, "skipped").value_or(0);
            run.seconds = number<double>(event, "seconds");
        }
    }
    return runs;
}

std::expected<std::string, std::string> find_run(std::span<const RunSummary> runs,
                                                 std::string_view prefix) {
    std::vector<std::string> found;
    for (const auto& run : runs) {
        if (run.run == prefix) {
            return run.run;
        }
        if (!prefix.empty() && run.run.starts_with(prefix)) {
            found.push_back(run.run);
        }
    }
    if (found.empty()) {
        return std::unexpected(std::format("no run is logged as {}", prefix));
    }
    if (found.size() > 1) {
        return std::unexpected(std::format("{} runs start with {}", found.size(), prefix));
    }
    return found.front();
}

std::string summary_line(const RunSummary& run, const std::chrono::time_zone& zone) {
    auto line = std::format("{}  {}  {}", local_time(run.started, zone), run.run, run.command);
    if (!run.targets.empty()) {
        line += " " + run.targets;
    }
    line += "  " + run.status;
    if (run.seconds) {
        line += std::format(": {} merged, {} uninstalled, {} failed, {} skipped in {}", run.merged,
                            run.uninstalled, run.failed, run.skipped, duration(*run.seconds));
    }
    return line;
}

std::string summary_fields(const RunSummary& run) {
    return std::format(
        "{}\t{:.0f}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", run.run, run.started, run.command, run.status,
        run.merged, run.uninstalled, run.failed, run.skipped,
        run.seconds ? std::format("{:.1f}", *run.seconds) : std::string{}, run.targets);
}

std::string event_line(const Event& event, const std::chrono::time_zone& zone) {
    return std::format("{}  {}", local_time(event.time, zone), event.message);
}

} // namespace egraph::log
