#include "pressure.hpp"

#include <charconv>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace egraph::pressure {

namespace {

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> found;
    for (const auto word : std::views::split(line, ' ')) {
        if (!word.empty()) {
            found.emplace_back(word.begin(), word.end());
        }
    }
    return found;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> found;
    for (const auto line : std::views::split(text, '\n')) {
        found.emplace_back(line.begin(), line.end());
    }
    return found;
}

template <class T> std::optional<T> number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return value;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

std::optional<CpuTimes> parse_stat(std::string_view text) {
    for (const auto line : lines(text)) {
        const auto fields = words(line);
        if (fields.empty() || fields.front() != "cpu") {
            continue;
        }
        // user nice system idle iowait irq softirq steal; guest time is already in user.
        std::vector<std::uint64_t> ticks;
        for (const auto field : fields | std::views::drop(1) | std::views::take(8)) {
            const auto value = number<std::uint64_t>(field);
            if (!value) {
                return std::nullopt;
            }
            ticks.push_back(*value);
        }
        if (ticks.size() < 4) {
            return std::nullopt;
        }
        ticks.resize(8, 0);
        const auto idle = ticks.at(3) + ticks.at(4);
        const auto busy =
            ticks.at(0) + ticks.at(1) + ticks.at(2) + ticks.at(5) + ticks.at(6) + ticks.at(7);
        return CpuTimes{.busy = busy, .total = busy + idle};
    }
    return std::nullopt;
}

std::size_t count_cpus(std::string_view text) {
    std::size_t count = 0;
    for (const auto line : lines(text)) {
        if (line.size() > 3 && line.starts_with("cpu") && line.at(3) >= '0' && line.at(3) <= '9') {
            ++count;
        }
    }
    return count;
}

std::optional<std::uint64_t> parse_meminfo(std::string_view text, std::string_view field) {
    for (const auto line : lines(text)) {
        const auto fields = words(line);
        if (fields.size() < 2 || fields.front().size() != field.size() + 1 ||
            !fields.front().starts_with(field) || !fields.front().ends_with(':')) {
            continue;
        }
        const auto value = number<std::uint64_t>(fields.at(1));
        if (!value) {
            return std::nullopt;
        }
        return fields.size() > 2 && fields.at(2) == "kB" ? *value * 1024 : *value;
    }
    return std::nullopt;
}

std::optional<double> parse_loadavg(std::string_view text) {
    const auto fields = words(text.substr(0, text.find('\n')));
    return fields.empty() ? std::nullopt : number<double>(fields.front());
}

std::optional<double> parse_psi(std::string_view text) {
    for (const auto line : lines(text)) {
        const auto fields = words(line);
        if (fields.empty() || fields.front() != "some") {
            continue;
        }
        for (const auto field : fields) {
            if (field.starts_with("avg10=")) {
                return number<double>(field.substr(6));
            }
        }
    }
    return std::nullopt;
}

Sample read_sample(const std::filesystem::path& proc) {
    const auto stat = read_text(proc / "stat");
    const auto meminfo = read_text(proc / "meminfo");
    return {.cpu = parse_stat(stat),
            .cpus = count_cpus(stat),
            .mem_total = parse_meminfo(meminfo, "MemTotal"),
            .mem_available = parse_meminfo(meminfo, "MemAvailable"),
            .load = parse_loadavg(read_text(proc / "loadavg")),
            .stalls = {.cpu = parse_psi(read_text(proc / "pressure/cpu")),
                       .memory = parse_psi(read_text(proc / "pressure/memory")),
                       .io = parse_psi(read_text(proc / "pressure/io"))}};
}

std::optional<double> cpu_busy(const Sample& before, const Sample& after) {
    if (!before.cpu || !after.cpu || after.cpu->total <= before.cpu->total ||
        after.cpu->busy < before.cpu->busy) {
        return std::nullopt;
    }
    return static_cast<double>(after.cpu->busy - before.cpu->busy) /
           static_cast<double>(after.cpu->total - before.cpu->total);
}

void History::add(const Sample& sample) {
    readings_.push_back({.cpu_busy = latest_ ? cpu_busy(*latest_, sample) : std::nullopt,
                         .mem_available = sample.mem_available,
                         .load = sample.load,
                         .stalls = sample.stalls});
    while (readings_.size() > capacity_) {
        readings_.pop_front();
    }
    latest_ = sample;
}

} // namespace egraph::pressure
