#include "usage.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace egraph::usage {

namespace {

template <class T> std::optional<T> number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

std::optional<Stat> parse_stat(std::string_view text) {
    // The name may hold anything, parentheses too; the fields start after the last one.
    const auto close = text.rfind(')');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    std::vector<std::string_view> fields;
    for (const auto field : std::views::split(text.substr(close + 1), ' ')) {
        if (!field.empty()) {
            fields.emplace_back(field.begin(), field.end());
        }
    }
    // From the state: ppid is the 2nd, cutime and cstime the 14th and 15th, rss the 22nd.
    if (fields.size() < 22) {
        return std::nullopt;
    }
    if (!fields.back().empty() && fields.back().back() == '\n') {
        fields.back().remove_suffix(1);
    }
    const auto ppid = number<std::int64_t>(fields.at(1));
    const auto cutime = number<std::uint64_t>(fields.at(13));
    const auto cstime = number<std::uint64_t>(fields.at(14));
    const auto rss = number<std::int64_t>(fields.at(21));
    if (!ppid || !cutime || !cstime || !rss) {
        return std::nullopt;
    }
    return Stat{.ppid = *ppid,
                .rss_pages = static_cast<std::uint64_t>(std::max<std::int64_t>(*rss, 0)),
                .children_ticks = *cutime + *cstime};
}

std::optional<Io> parse_io(std::string_view text) {
    std::map<std::string_view, std::uint64_t> values;
    for (const auto each : std::views::split(text, '\n')) {
        const std::string_view line{each.begin(), each.end()};
        const auto colon = line.find(": ");
        if (colon == std::string_view::npos) {
            continue;
        }
        const auto value = number<std::uint64_t>(line.substr(colon + 2));
        if (!value) {
            return std::nullopt;
        }
        values.insert_or_assign(line.substr(0, colon), *value);
    }
    const auto read = values.find("read_bytes");
    const auto written = values.find("write_bytes");
    if (read == values.end() || written == values.end()) {
        return std::nullopt;
    }
    // Dirty pages truncated away before writeback, as a build's temporary files often are, never
    // reached storage.
    const auto cancelled = values.find("cancelled_write_bytes");
    const auto never = cancelled == values.end() ? 0 : cancelled->second;
    return Io{.read_bytes = read->second,
              .write_bytes = written->second > never ? written->second - never : 0};
}

std::optional<Counters> read_counters(std::int64_t pid, std::uint64_t ticks,
                                      const std::filesystem::path& proc) {
    const auto dir = proc / std::to_string(pid);
    const auto text = read_text(dir / "stat");
    const auto stat = text ? parse_stat(*text) : std::nullopt;
    if (!stat || ticks == 0) {
        return std::nullopt;
    }
    Counters counters{.children_cpu =
                          static_cast<double>(stat->children_ticks) / static_cast<double>(ticks)};
    if (const auto io_text = read_text(dir / "io")) {
        if (const auto io = parse_io(*io_text)) {
            counters.read_bytes = io->read_bytes;
            counters.write_bytes = io->write_bytes;
        }
    }
    return counters;
}

std::vector<Process> read_processes(const std::filesystem::path& proc) {
    std::vector<Process> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{proc, error}) {
        const auto name = entry.path().filename().string();
        const auto pid = number<std::int64_t>(name);
        if (!pid) {
            continue;
        }
        // Gone since the listing: left out.
        const auto text = read_text(entry.path() / "stat");
        const auto stat = text ? parse_stat(*text) : std::nullopt;
        if (stat) {
            found.push_back({.pid = *pid, .ppid = stat->ppid, .rss_pages = stat->rss_pages});
        }
    }
    return found;
}

std::uint64_t pages_under(const std::vector<Process>& processes, std::int64_t pid) {
    std::multimap<std::int64_t, std::size_t> children;
    for (std::size_t at = 0; at < processes.size(); ++at) {
        children.emplace(processes.at(at).ppid, at);
    }
    // /proc is read a process at a time, so a pid reused meanwhile could make a loop.
    std::set<std::int64_t> seen{pid};
    std::uint64_t pages = 0;
    std::vector<std::int64_t> parents{pid};
    while (!parents.empty()) {
        const auto parent = parents.back();
        parents.pop_back();
        const auto [first, last] = children.equal_range(parent);
        for (const auto at : std::ranges::subrange(first, last) | std::views::values) {
            const auto& child = processes.at(at);
            if (seen.insert(child.pid).second) {
                pages += child.rss_pages;
                parents.push_back(child.pid);
            }
        }
    }
    return pages;
}

} // namespace egraph::usage
