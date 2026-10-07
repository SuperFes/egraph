#include "history.hpp"

#include "atom.hpp"
#include "check.hpp"
#include "human.hpp"
#include "json.hpp"
#include "version.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <ostream>
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

// A package's change from one store to another, by its ids in each.
struct Change {
    std::string_view kind;
    std::optional<std::uint32_t> before{};
    std::optional<std::uint32_t> after{};
};

std::vector<std::string_view> enabled(const Store& store, const Package& pkg) {
    std::vector<std::string_view> flags;
    for (const auto id : store.ids_in(pkg.use)) {
        flags.push_back(store.string(id));
    }
    std::ranges::sort(flags);
    return flags;
}

// new, upgrade, downgrade (another version replacing one gone from its slot), rebuild (the same
// cpv merged again, or with other flags), uninstall; in after's order, then before's.
std::vector<Change> changes(const Store& before, const Store& after) {
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

    std::vector<Change> found;
    std::set<std::uint32_t> replaced;
    for (std::uint32_t id = 0; id < after.packages.size(); ++id) {
        const auto& pkg = after.packages.at(id);
        if (const auto same = before_ids.find(after.string(pkg.cpv)); same != before_ids.end()) {
            const auto& old = before.packages.at(same->second);
            if (old.counter != pkg.counter || enabled(before, old) != enabled(after, pkg)) {
                found.push_back({.kind = "rebuild", .before = same->second, .after = id});
            }
            continue;
        }
        Change change{.kind = "new", .after = id};
        const auto slot = before_slots.find({after.string(pkg.cp), after.string(pkg.slot)});
        const auto in_slot =
            slot != before_slots.end() ? std::span{slot->second} : std::span<const std::uint32_t>{};
        for (const auto old_id : in_slot) {
            const auto& old = before.packages.at(old_id);
            if (after_cpvs.contains(before.string(old.cpv)) || replaced.contains(old_id)) {
                continue;
            }
            const auto from = version(before, old);
            const auto to = version(after, pkg);
            change.kind = from && to && vercmp(*to, *from) < 0 ? "downgrade" : "upgrade";
            change.before = old_id;
            replaced.insert(old_id);
            break;
        }
        found.push_back(change);
    }
    for (const auto& [cpv, id] : before_ids) {
        if (!after_cpvs.contains(cpv) && !replaced.contains(id)) {
            found.push_back({.kind = "uninstall", .before = id});
        }
    }
    return found;
}

// Flags on in after but not before ("+flag"), and off ("-flag"), by flag.
std::vector<std::string> flag_changes(const std::vector<std::string_view>& before,
                                      const std::vector<std::string_view>& after) {
    std::vector<std::pair<std::string_view, char>> flipped;
    flipped.reserve(before.size() + after.size());
    for (const auto flag : after) {
        if (!std::ranges::binary_search(before, flag)) {
            flipped.emplace_back(flag, '+');
        }
    }
    for (const auto flag : before) {
        if (!std::ranges::binary_search(after, flag)) {
            flipped.emplace_back(flag, '-');
        }
    }
    std::ranges::sort(flipped);
    std::vector<std::string> flags;
    flags.reserve(flipped.size());
    for (const auto& [flag, sign] : flipped) {
        flags.push_back(std::format("{}{}", sign, flag));
    }
    return flags;
}

// An age: a count of hours, days or weeks.
std::optional<std::chrono::seconds> age(std::string_view text) {
    using namespace std::chrono;
    if (text.size() < 2) {
        return std::nullopt;
    }
    const auto count = number(text.substr(0, text.size() - 1));
    if (!count) {
        return std::nullopt;
    }
    switch (text.back()) {
    case 'h':
        return hours{*count};
    case 'd':
        return days{*count};
    case 'w':
        return weeks{*count};
    default:
        return std::nullopt;
    }
}

// Where a package's kind of change comes in a diff.
int kind_rank(std::string_view kind) {
    return kind == "upgrade"     ? 0
           : kind == "downgrade" ? 1
           : kind == "rebuild"   ? 2
           : kind == "new"       ? 3
                                 : 4;
}

// YYYY-mm-dd
std::optional<std::chrono::year_month_day> date(std::string_view text) {
    if (text.size() != 10 || text.at(4) != '-' || text.at(7) != '-') {
        return std::nullopt;
    }
    const auto year = number(text.substr(0, 4));
    const auto month = number(text.substr(5, 2));
    const auto day = number(text.substr(8, 2));
    if (!year || !month || !day) {
        return std::nullopt;
    }
    const std::chrono::year_month_day parsed{std::chrono::year{*year},
                                             std::chrono::month{static_cast<unsigned>(*month)},
                                             std::chrono::day{static_cast<unsigned>(*day)}};
    return parsed.ok() ? std::optional{parsed} : std::nullopt;
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

std::string generation_name(Seconds ended) {
    return std::format("installed-{:%Y%m%dT%H%M%SZ}.egraph", ended);
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
    std::vector<HistoryEvent> events;
    for (const auto& change : changes(before, after)) {
        if (!change.after) {
            if (change.before) {
                events.push_back(
                    {.time = now,
                     .event = "uninstalled",
                     .cpv = std::string{before.string(before.packages.at(*change.before).cpv)}});
            }
            continue;
        }
        const auto& pkg = after.packages.at(*change.after);
        const Seconds time = pkg.merged != 0 ? Seconds{std::chrono::seconds{pkg.merged}} : now;
        HistoryEvent event{.time = time,
                           .event = change.kind == "new"         ? "merged"
                                    : change.kind == "upgrade"   ? "upgraded"
                                    : change.kind == "downgrade" ? "downgraded"
                                                                 : "rebuilt",
                           .cpv = std::string{after.string(pkg.cpv)}};
        if (change.before && change.kind != "rebuild") {
            event.from = before.string(before.packages.at(*change.before).cpv);
        }
        events.push_back(std::move(event));
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

std::vector<HistoryEvent> parse_history(std::string_view text) {
    std::vector<HistoryEvent> events;
    for (const auto line : std::views::split(text, '\n')) {
        const auto json = nlohmann::json::parse(std::string_view{line}, nullptr, false);
        if (!json.is_object()) {
            continue;
        }
        const auto cpv = json.find("cpv");
        const auto event = json.find("event");
        const auto time = json.find("time");
        if (cpv == json.end() || !cpv->is_string() || event == json.end() || !event->is_string() ||
            time == json.end() || !time->is_number_integer()) {
            continue;
        }
        HistoryEvent read{.time = Seconds{std::chrono::seconds{time->get<std::int64_t>()}},
                          .event = event->get<std::string>(),
                          .cpv = cpv->get<std::string>()};
        if (const auto from = json.find("from"); from != json.end() && from->is_string()) {
            read.from = from->get<std::string>();
        }
        events.push_back(std::move(read));
    }
    return events;
}

std::expected<HistoryQuery, std::string> parse_history_query(std::span<const std::string> arguments,
                                                             Seconds now,
                                                             const std::chrono::time_zone& zone) {
    HistoryQuery query;
    std::optional<std::string_view> when;
    for (const auto& argument : arguments) {
        if (const auto base = parse_diff_base(argument, now, zone); base && base->at) {
            if (when) {
                return std::unexpected(
                    std::format("history takes one age or date, not {} and {}", *when, argument));
            }
            when = argument;
            query.since = base->at;
        } else {
            query.packages.push_back(argument);
        }
    }
    return query;
}

namespace {

std::string_view cp_of(std::string_view cpv) {
    const auto parts = split_cpv(cpv);
    return cpv.substr(0, parts.category.size() + 1 + parts.name.size());
}

// A package argument as history matches it: an exact cpv, or an atom by cp and version.
struct LoggedPackage {
    std::string cpv;
    std::optional<Atom> atom;

    [[nodiscard]] bool matches(std::string_view logged) const {
        if (logged.empty()) {
            return false;
        }
        if (!atom) {
            return logged == cpv;
        }
        const auto version = parse_version(split_cpv(logged).version);
        return version && egraph::matches(*atom, cp_of(logged), *version, "", "", "");
    }
};

std::expected<LoggedPackage, std::string> logged_package(std::string_view text) {
    // An operator starts a versioned atom, never a cpv.
    const bool plain = !text.empty() && std::isalnum(static_cast<unsigned char>(text.front())) != 0;
    if (const auto parts = split_cpv(text);
        plain && !parts.category.empty() && !parts.version.empty()) {
        return LoggedPackage{.cpv = std::string{text}, .atom = std::nullopt};
    }
    auto atom = parse_atom(text);
    if (!atom) {
        return std::unexpected(atom.error());
    }
    if (atom->slot || atom->sub_slot || atom->repo || !atom->use.empty()) {
        return std::unexpected(std::format("{}: history matches packages by name and version; the "
                                           "log holds no slots, repositories or USE",
                                           text));
    }
    return LoggedPackage{.cpv = {}, .atom = std::move(*atom)};
}

} // namespace

std::expected<std::vector<HistoryEvent>, std::string>
selected_events(std::span<const HistoryEvent> events, const HistoryQuery& query) {
    std::vector<LoggedPackage> packages;
    for (const auto& text : query.packages) {
        auto package = logged_package(text);
        if (!package) {
            return std::unexpected(std::move(package.error()));
        }
        packages.push_back(std::move(*package));
    }
    std::vector<HistoryEvent> selected;
    for (const auto& event : events) {
        if (query.since && event.time < *query.since) {
            continue;
        }
        if (!packages.empty() && std::ranges::none_of(packages, [&event](const auto& package) {
                return package.matches(event.cpv) || package.matches(event.from);
            })) {
            continue;
        }
        selected.push_back(event);
    }
    return selected;
}

std::vector<HistoryEvent> arrivals(std::span<const HistoryEvent> events) {
    std::set<std::string_view> arrived;
    std::vector<HistoryEvent> found;
    for (const auto& event : events) {
        if (event.event != "uninstalled" && arrived.insert(cp_of(event.cpv)).second) {
            found.push_back(event);
        }
    }
    return found;
}

std::vector<std::string> event_records(std::span<const HistoryEvent> events,
                                       const std::chrono::time_zone& zone) {
    std::vector<std::string> records;
    records.reserve(events.size());
    for (const auto& event : events) {
        const auto time =
            std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::zoned_time{&zone, event.time});
        const auto& kind = event.event;
        const std::string_view change = kind == "merged"       ? "new"
                                        : kind == "upgraded"   ? "upgrade"
                                        : kind == "downgraded" ? "downgrade"
                                        : kind == "rebuilt"    ? "rebuild"
                                                               : "uninstall";
        const std::string_view before = change == "new"      ? std::string_view{}
                                        : event.from.empty() ? std::string_view{event.cpv}
                                                             : std::string_view{event.from};
        const std::string_view after = change == "uninstall" ? std::string_view{} : event.cpv;
        records.push_back(std::format("{}\t{}\t{}\t{}\t", time, before, change, after));
    }
    return records;
}

bool history_changed(const Store& before, const Store& after) {
    return !drift(before, after).empty() || roots_of(before) != roots_of(after);
}

std::expected<DiffBase, std::string> parse_diff_base(std::string_view text, Seconds now,
                                                     const std::chrono::time_zone& zone) {
    if (generation_time(text)) {
        return DiffBase{.generation = std::string{text}};
    }
    if (const auto back = age(text)) {
        return DiffBase{.at = now - *back};
    }
    if (const auto day = date(text)) {
        return DiffBase{.at = std::chrono::floor<std::chrono::seconds>(zone.to_sys(
                            std::chrono::local_days{*day}, std::chrono::choose::earliest))};
    }
    return std::unexpected(std::format(
        "expected an age (12h, 3d, 2w), a date (2026-09-30) or a generation's name, not {}", text));
}

std::optional<Seconds> generation_at(std::span<const Seconds> generations,
                                     std::optional<Seconds> at) {
    std::optional<Seconds> chosen;
    for (const auto ended : generations) {
        if (!at) {
            chosen = std::max(chosen.value_or(ended), ended);
        } else if (ended > *at) {
            chosen = std::min(chosen.value_or(ended), ended);
        }
    }
    return chosen;
}

std::vector<Difference> differences(const Store& before, const Store& after) {
    std::vector<Difference> found;
    for (const auto& change : changes(before, after)) {
        Difference difference{.kind = std::string{change.kind}};
        if (change.before) {
            difference.before = before.string(before.packages.at(*change.before).cpv);
        }
        if (change.after) {
            difference.after = after.string(after.packages.at(*change.after).cpv);
        }
        if (change.before && change.after) {
            difference.use = flag_changes(enabled(before, before.packages.at(*change.before)),
                                          enabled(after, after.packages.at(*change.after)));
        }
        found.push_back(std::move(difference));
    }
    std::ranges::sort(found, [](const Difference& a, const Difference& b) {
        return std::tuple{kind_rank(a.kind), a.after.empty() ? a.before : a.after} <
               std::tuple{kind_rank(b.kind), b.after.empty() ? b.before : b.after};
    });

    // Each set's atoms, by value.
    using Atoms = std::set<std::pair<std::string_view, std::string_view>>;
    const auto atoms = [](const Store& store) {
        Atoms all;
        for (const auto& root : store.roots) {
            all.emplace(store.string(root.set), store.string(root.atom));
        }
        return all;
    };
    const auto old_atoms = atoms(before);
    const auto new_atoms = atoms(after);
    std::vector<std::tuple<std::string_view, std::string_view, std::string_view>> roots;
    for (const auto& [set, atom] : new_atoms) {
        if (!old_atoms.contains({set, atom})) {
            roots.emplace_back(set, "added", atom);
        }
    }
    for (const auto& [set, atom] : old_atoms) {
        if (!new_atoms.contains({set, atom})) {
            roots.emplace_back(set, "removed", atom);
        }
    }
    std::ranges::sort(roots);
    for (const auto& [set, kind, atom] : roots) {
        found.push_back({.kind = std::string{kind},
                         .before = std::format("@{}", set),
                         .after = std::string{atom}});
    }
    return found;
}

namespace {

bool is_atom(const Difference& change) {
    return change.kind == "added" || change.kind == "removed";
}

} // namespace

std::vector<std::string> difference_lines(std::span<const Difference> changes) {
    std::vector<std::string> lines;
    for (const auto& change : changes) {
        auto line = std::format("{}\t{}\t{}", change.before, change.kind, change.after);
        if (!is_atom(change)) {
            line += '\t';
            line += change.use | std::views::join_with(' ') | std::ranges::to<std::string>();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

void write_differences_json(std::ostream& out, std::span<const Difference> changes, Seconds since,
                            std::optional<std::string_view> generation) {
    out << R"({"changes":[)";
    bool first = true;
    for (const auto& change : changes) {
        out << (first ? "{" : ",{");
        first = false;
        if (is_atom(change)) {
            out << R"("atom":)";
            write_json_string(out, change.after);
            out << R"(,"change":")" << change.kind << R"(","set":)";
            write_json_string(out, change.before);
            out << '}';
            continue;
        }
        if (!change.after.empty()) {
            out << R"("after":)";
            write_json_string(out, change.after);
            out << ',';
        }
        if (!change.before.empty()) {
            out << R"("before":)";
            write_json_string(out, change.before);
            out << ',';
        }
        out << R"("change":")" << change.kind << R"(","use":[)";
        bool first_flag = true;
        for (const auto& flag : change.use) {
            out << (first_flag ? "" : ",");
            first_flag = false;
            write_json_string(out, flag);
        }
        out << "]}";
    }
    out << "],";
    if (generation) {
        out << R"("generation":)";
        write_json_string(out, *generation);
        out << ',';
    }
    out << R"("since":)" << since.time_since_epoch().count() << "}\n";
}

} // namespace egraph
