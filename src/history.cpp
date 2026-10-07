#include "history.hpp"

#include "check.hpp"
#include "json.hpp"
#include "version.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace egraph {

namespace {

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") + 1 - first);
}

// Digits only, all of text, as a number.
std::optional<int> number(std::string_view text) {
    int value = 0;
    const auto [at, error] = std::from_chars(text.begin(), text.end(), value);
    if (text.empty() || error != std::errc{} || at != text.end() ||
        !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    return value;
}

using Roots = std::vector<std::tuple<std::string_view, std::string_view, std::string_view>>;

Roots roots_of(const Store& store) {
    Roots roots;
    for (const auto& root : store.roots) {
        roots.emplace_back(store.string(root.set), store.string(root.atom), store.string(root.via));
    }
    std::ranges::sort(roots);
    return roots;
}

} // namespace

std::expected<Settings, std::string> parse_settings(std::string_view text) {
    Settings settings;
    std::size_t line_number = 0;
    for (const auto line : std::views::split(text, '\n')) {
        ++line_number;
        auto entry = std::string_view{line.begin(), line.end()};
        entry = trimmed(entry.substr(0, entry.find('#')));
        if (entry.empty()) {
            continue;
        }
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(std::format("line {}: expected key = value", line_number));
        }
        const auto key = trimmed(entry.substr(0, equals));
        const auto value = trimmed(entry.substr(equals + 1));
        if (key == "history_days") {
            const auto days = number(value);
            if (!days) {
                return std::unexpected(std::format(
                    "line {}: history_days takes a number of days, not {}", line_number, value));
            }
            settings.history_days = *days;
        } else {
            return std::unexpected(std::format("line {}: unknown setting {}", line_number, key));
        }
    }
    return settings;
}

std::filesystem::path settings_path(const std::filesystem::path& config_root) {
    return config_root / "etc/egraph/egraph.conf";
}

std::expected<Settings, std::string> read_settings(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return Settings{};
    }
    std::ifstream in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    if (!in) {
        return std::unexpected(std::format("{}: cannot read it", path.string()));
    }
    auto settings = parse_settings(text.str());
    if (!settings) {
        return std::unexpected(std::format("{}: {}", path.string(), settings.error()));
    }
    return settings;
}

std::filesystem::path history_directory(const std::filesystem::path& root,
                                        const std::filesystem::path& eprefix) {
    return root / eprefix.relative_path() / "var/lib/egraph";
}

std::string generation_name(Seconds built) {
    return std::format("installed-{:%Y%m%dT%H%M%SZ}.egraph", built);
}

std::optional<Seconds> generation_time(std::string_view name) {
    constexpr std::string_view prefix = "installed-";
    constexpr std::string_view suffix = ".egraph";
    // YYYYmmddTHHMMSSZ
    constexpr std::size_t stamp = 16;
    if (name.size() != prefix.size() + stamp + suffix.size() || !name.starts_with(prefix) ||
        !name.ends_with(suffix)) {
        return std::nullopt;
    }
    const auto text = name.substr(prefix.size(), stamp);
    const auto field = [&text](std::size_t at, std::size_t size) {
        return number(text.substr(at, size));
    };
    const auto year = field(0, 4);
    const auto month = field(4, 2);
    const auto day = field(6, 2);
    const auto hour = field(9, 2);
    const auto minute = field(11, 2);
    const auto second = field(13, 2);
    if (text.at(8) != 'T' || text.at(15) != 'Z' || !year || !month || !day || !hour || !minute ||
        !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

std::vector<Seconds> thinned(std::span<const Seconds> generations, Seconds now, int days) {
    using std::chrono::hours;
    const auto whole = hours{24};
    const auto oldest = whole * days;
    // The newest generation of each day past the last one.
    std::map<std::chrono::sys_days, Seconds> newest;
    for (const auto built : generations) {
        if (now - built > whole) {
            auto& kept = newest.try_emplace(std::chrono::floor<std::chrono::days>(built), built)
                             .first->second;
            kept = std::max(kept, built);
        }
    }
    std::vector<Seconds> gone;
    for (const auto built : generations) {
        const auto age = now - built;
        const bool kept =
            days > 0 && age <= oldest &&
            (age <= whole || newest.at(std::chrono::floor<std::chrono::days>(built)) == built);
        if (!kept) {
            gone.push_back(built);
        }
    }
    return gone;
}

std::vector<HistoryEvent> history_events(const Store& before, const Store& after, Seconds now) {
    using Slot = std::pair<std::string_view, std::string_view>;
    std::map<std::string_view, std::uint32_t> before_ids;
    std::map<Slot, std::vector<std::uint32_t>> before_slots;
    for (std::uint32_t id = 0; id < before.packages.size(); ++id) {
        const auto& pkg = before.packages.at(id);
        before_ids.emplace(before.string(pkg.cpv), id);
        before_slots[{before.string(pkg.cp), before.string(pkg.slot)}].push_back(id);
    }
    std::set<std::string_view> after_cpvs;
    for (const auto& pkg : after.packages) {
        after_cpvs.insert(after.string(pkg.cpv));
    }
    const auto version = [](const Store& store, const Package& pkg) {
        return parse_version(store.string(pkg.cpv).substr(store.string(pkg.cp).size() + 1));
    };

    std::vector<HistoryEvent> events;
    std::set<std::uint32_t> replaced;
    for (const auto& pkg : after.packages) {
        const auto cpv = after.string(pkg.cpv);
        const Seconds time = pkg.merged != 0 ? Seconds{std::chrono::seconds{pkg.merged}} : now;
        if (const auto found = before_ids.find(cpv); found != before_ids.end()) {
            if (before.packages.at(found->second).counter != pkg.counter) {
                events.push_back({.time = time, .event = "rebuilt", .cpv = std::string{cpv}});
            }
            continue;
        }
        HistoryEvent event{.time = time, .event = "merged", .cpv = std::string{cpv}};
        const auto slot = before_slots.find({after.string(pkg.cp), after.string(pkg.slot)});
        const auto in_slot =
            slot != before_slots.end() ? std::span{slot->second} : std::span<const std::uint32_t>{};
        for (const auto id : in_slot) {
            const auto& old = before.packages.at(id);
            if (after_cpvs.contains(before.string(old.cpv)) || replaced.contains(id)) {
                continue;
            }
            const auto from = version(before, old);
            const auto to = version(after, pkg);
            event.event = from && to && vercmp(*to, *from) < 0 ? "downgraded" : "upgraded";
            event.from = before.string(old.cpv);
            replaced.insert(id);
            break;
        }
        events.push_back(std::move(event));
    }
    for (const auto& [cpv, id] : before_ids) {
        if (!after_cpvs.contains(cpv) && !replaced.contains(id)) {
            events.push_back({.time = now, .event = "uninstalled", .cpv = std::string{cpv}});
        }
    }
    std::ranges::sort(events, [](const HistoryEvent& a, const HistoryEvent& b) {
        return std::tuple{a.time, a.event == "uninstalled", a.cpv} <
               std::tuple{b.time, b.event == "uninstalled", b.cpv};
    });
    return events;
}

std::string event_line(const HistoryEvent& event) {
    std::ostringstream out;
    out << R"({"cpv":)";
    write_json_string(out, event.cpv);
    out << R"(,"event":")" << event.event << '"';
    if (!event.from.empty()) {
        out << R"(,"from":)";
        write_json_string(out, event.from);
    }
    out << R"(,"time":)" << event.time.time_since_epoch().count() << '}';
    return std::move(out).str();
}

bool history_changed(const Store& before, const Store& after) {
    return !drift(before, after).empty() || roots_of(before) != roots_of(after);
}

} // namespace egraph
