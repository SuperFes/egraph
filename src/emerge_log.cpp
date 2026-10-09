#include "emerge_log.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <utility>

namespace egraph {

namespace {

// A build between its ">>> emerge" line and its completion.
struct Pending {
    double start = 0;
    std::string root{};
    std::optional<std::uint32_t> jobs{};
    bool compiled = false;
    bool binary = false;
    std::optional<double> merging{};
};

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(' ');
    return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

std::string with_slash(std::string_view root) {
    std::string found{root};
    if (!found.ends_with('/')) {
        found += '/';
    }
    return found;
}

template <class Number> std::optional<Number> number(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return value;
}

// "1 of 5) app-misc/a-1 to /": the count, the cpv and the root.
struct Named {
    std::string_view count;
    std::string_view cpv;
    std::string_view root;
};
std::optional<Named> merge_named(std::string_view rest) {
    const auto close = rest.find(") ");
    const auto to = rest.find(" to ", close == std::string_view::npos ? 0 : close);
    if (close == std::string_view::npos || to == std::string_view::npos) {
        return std::nullopt;
    }
    return Named{.count = rest.substr(0, close),
                 .cpv = rest.substr(close + 2, to - close - 2),
                 .root = rest.substr(to + 4)};
}

// "1 of 5) Compiling/Merging (app-misc/a-1::path)": the count, what it does and the cpv.
struct Stage {
    std::string_view count;
    std::string_view action;
    std::string_view cpv;
};
std::optional<Stage> stage_named(std::string_view rest) {
    const auto close = rest.find(") ");
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    const auto open = rest.find(" (", close + 2);
    const auto repo = rest.find("::", open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || repo == std::string_view::npos) {
        return std::nullopt;
    }
    return Stage{.count = rest.substr(0, close),
                 .action = rest.substr(close + 2, open - close - 2),
                 .cpv = rest.substr(open + 2, repo - open - 2)};
}

// The jobs a session's options allow: --jobs=N, none for --jobs alone, else one.
std::optional<std::uint32_t> session_jobs(std::string_view options) {
    for (const auto word : std::views::split(options, ' ')) {
        const std::string_view each{word.begin(), word.end()};
        if (each == "--jobs") {
            return std::nullopt;
        }
        if (each.starts_with("--jobs=")) {
            return number<std::uint32_t>(each.substr(7)).value_or(1);
        }
    }
    return 1;
}

} // namespace

std::filesystem::path emerge_log_path(const std::filesystem::path& eprefix) {
    return eprefix / "var/log/emerge.log";
}

std::vector<BuildRecord> emerge_log_builds(std::string_view text, std::string_view eroot) {
    const auto wanted = with_slash(eroot);
    std::optional<std::uint32_t> jobs = 1;
    // By count and cpv, as emerge names a build in each of its lines.
    std::map<std::string, Pending, std::less<>> pending;
    std::vector<BuildRecord> found;
    for (const auto part : std::views::split(text, '\n')) {
        const std::string_view line{part.begin(), part.end()};
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const auto time = number<std::int64_t>(line.substr(0, colon));
        if (!time) {
            continue;
        }
        const auto at = static_cast<double>(*time);
        const auto message = trimmed(line.substr(colon + 1));
        if (message.starts_with("*** emerge ")) {
            jobs = session_jobs(message.substr(11));
        } else if (message.starts_with(">>> emerge (")) {
            if (const auto named = merge_named(message.substr(12))) {
                pending.insert_or_assign(
                    std::format("{} {}", named->count, named->cpv),
                    Pending{.start = at, .root = with_slash(named->root), .jobs = jobs});
            }
        } else if (message.starts_with("=== (")) {
            const auto stage = stage_named(message.substr(5));
            if (!stage) {
                continue;
            }
            const auto build = pending.find(std::format("{} {}", stage->count, stage->cpv));
            if (build == pending.end()) {
                continue;
            }
            if (stage->action.starts_with("Compiling/")) {
                build->second.compiled = true;
            } else if (stage->action == "Merging") {
                build->second.merging = at;
            } else if (stage->action == "Merging Binary") {
                build->second.binary = true;
            }
        } else if (message.starts_with("::: completed emerge (")) {
            const auto named = merge_named(message.substr(22));
            if (!named) {
                continue;
            }
            const auto build = pending.find(std::format("{} {}", named->count, named->cpv));
            if (build == pending.end()) {
                continue;
            }
            const auto done = std::move(build->second);
            pending.erase(build);
            if (!done.compiled || done.binary || !done.merging || done.root != wanted) {
                continue;
            }
            found.push_back({.origin = std::string{emerge_log_origin},
                             .start = done.start,
                             .cpv = std::string{named->cpv},
                             .build_seconds = *done.merging - done.start,
                             .merge_seconds = at - *done.merging,
                             .jobs = done.jobs});
        }
    }
    std::ranges::stable_sort(found, std::less<>{}, &BuildRecord::start);
    return found;
}

Imported imported_builds(std::string_view history, std::span<const BuildRecord> records,
                         std::size_t keep) {
    std::string unread;
    // Each record's start and line, those of the history as they are.
    std::vector<std::pair<double, std::string>> lines;
    std::vector<BuildRecord> held;
    for (const auto part : std::views::split(history, '\n')) {
        const std::string_view line{part.begin(), part.end()};
        if (line.empty()) {
            continue;
        }
        if (auto record = parse_build_record(line)) {
            lines.emplace_back(record->start, std::string{line});
            held.push_back(std::move(*record));
        } else {
            unread += line;
            unread += '\n';
        }
    }
    std::set<std::string> added;
    for (const auto& record : records) {
        const bool known = std::ranges::any_of(held, [&](const BuildRecord& other) {
            return other.cpv == record.cpv &&
                   std::abs(other.start - record.start) <= import_tolerance;
        });
        if (known) {
            continue;
        }
        auto line = build_record_json(record);
        lines.emplace_back(record.start, line);
        added.insert(std::move(line));
        held.push_back(record);
    }
    std::ranges::stable_sort(lines, std::less<>{}, &std::pair<double, std::string>::first);
    std::string ordered = std::move(unread);
    for (const auto& [start, line] : lines) {
        ordered += line;
        ordered += '\n';
    }
    Imported imported{.text = pruned_builds(ordered, keep)};
    for (const auto part : std::views::split(imported.text, '\n')) {
        imported.added += added.contains(std::string{part.begin(), part.end()}) ? 1U : 0U;
    }
    return imported;
}

} // namespace egraph
