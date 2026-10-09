#include "build_views.hpp"

#include "human.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <utility>

namespace egraph {

namespace {

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values.at(middle)
                                  : (values.at(middle - 1) + values.at(middle)) / 2;
}

std::string cp_of(std::string_view cpv) {
    const auto parts = split_cpv(cpv);
    return std::format("{}/{}", parts.category, parts.name);
}

std::string seconds_or_empty(std::optional<double> seconds) {
    return seconds ? std::format("{:.1f}", *seconds) : std::string{};
}

// The local day a build started on.
std::chrono::local_days start_day(const BuildRecord& record, const std::chrono::time_zone& zone) {
    using namespace std::chrono;
    const sys_seconds start{seconds{static_cast<std::int64_t>(record.start)}};
    return floor<days>(zone.to_local(start));
}

// The first day of the period holding day.
std::chrono::local_days period_start(std::chrono::local_days day, BuildPeriod period) {
    using namespace std::chrono;
    if (period == BuildPeriod::week) {
        return day - (weekday{day} - Monday);
    }
    const year_month_day date{day};
    return local_days{date.year() / date.month() / 1};
}

std::chrono::local_days next_period(std::chrono::local_days start, BuildPeriod period) {
    using namespace std::chrono;
    if (period == BuildPeriod::week) {
        return start + weeks{1};
    }
    const year_month_day date{start};
    return local_days{date.year() / date.month() / 1 + months{1}};
}

} // namespace

std::vector<PackageTimes> slowest_packages(std::span<const BuildRecord> records) {
    std::map<std::string, PackageTimes> by_cp;
    std::map<std::string, std::vector<double>> cpus;
    for (const auto& record : records) {
        auto cp = cp_of(record.cpv);
        auto& times = by_cp.try_emplace(cp, PackageTimes{.cp = cp}).first->second;
        if (record.outcome == BuildRecord::Outcome::failed) {
            ++times.failed;
            continue;
        }
        times.seconds.push_back(seconds_building(record));
        if (record.cpu_seconds) {
            cpus[cp].push_back(*record.cpu_seconds);
        }
        if (record.peak_memory) {
            times.peak_memory = std::max(times.peak_memory.value_or(0), *record.peak_memory);
        }
    }
    std::vector<PackageTimes> found;
    for (auto& [cp, times] : by_cp) {
        if (times.seconds.empty()) {
            continue;
        }
        times.median = median(times.seconds);
        if (const auto cpu = cpus.find(cp); cpu != cpus.end()) {
            times.cpu = median(cpu->second);
        }
        found.push_back(std::move(times));
    }
    std::ranges::stable_sort(found, std::greater{}, &PackageTimes::median);
    return found;
}

std::string package_times_fields(const PackageTimes& times) {
    std::string seconds;
    for (const auto each : times.seconds) {
        seconds += std::format("{}{:.1f}", seconds.empty() ? "" : " ", each);
    }
    return std::format("{}\t{}\t{}\t{:.1f}\t{}\t{}\t{}", times.cp, times.seconds.size(),
                       times.failed, times.median, seconds_or_empty(times.cpu),
                       times.peak_memory ? std::to_string(*times.peak_memory) : std::string{},
                       seconds);
}

std::vector<PeriodTotal> period_totals(std::span<const BuildRecord> records, BuildPeriod period,
                                       const std::chrono::time_zone& zone) {
    std::map<std::chrono::local_days, PeriodTotal> by_start;
    for (const auto& record : records) {
        const auto start = period_start(start_day(record, zone), period);
        auto& total =
            by_start.try_emplace(start, PeriodTotal{.start = std::chrono::year_month_day{start}})
                .first->second;
        ++total.builds;
        if (record.outcome == BuildRecord::Outcome::failed) {
            ++total.failed;
        }
        total.seconds += record.build_seconds + record.merge_seconds.value_or(0);
        if (record.cpu_seconds) {
            total.cpu = total.cpu.value_or(0) + *record.cpu_seconds;
        }
    }
    std::vector<PeriodTotal> found;
    if (by_start.empty()) {
        return found;
    }
    const auto last = by_start.rbegin()->first;
    for (auto start = by_start.begin()->first; start <= last; start = next_period(start, period)) {
        const auto total = by_start.find(start);
        found.push_back(total == by_start.end()
                            ? PeriodTotal{.start = std::chrono::year_month_day{start}}
                            : total->second);
    }
    return found;
}

std::string period_fields(const PeriodTotal& total) {
    return std::format("{:%F}\t{}\t{}\t{:.1f}\t{}", std::chrono::sys_days{total.start},
                       total.builds, total.failed, total.seconds, seconds_or_empty(total.cpu));
}

} // namespace egraph
