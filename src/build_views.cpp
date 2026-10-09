#include "build_views.hpp"

#include "human.hpp"
#include "version.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
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

namespace {

// The counts as tallies, most first, then by name.
std::vector<Tally> tallies(const std::map<std::string, std::size_t>& counts) {
    std::vector<Tally> found;
    found.reserve(counts.size());
    for (const auto& [name, count] : counts) {
        found.push_back({.name = name, .count = count});
    }
    std::ranges::stable_sort(found, std::greater{}, &Tally::count);
    return found;
}

std::string tally_text(const std::vector<Tally>& found) {
    std::string text;
    for (const auto& each : found) {
        text += std::format("{}{}:{}", text.empty() ? "" : " ", each.name, each.count);
    }
    return text;
}

std::string joined(const std::vector<std::string>& words) {
    std::string text;
    for (const auto& word : words) {
        text += text.empty() ? word : " " + word;
    }
    return text;
}

// The flags of flags that from lacks.
std::vector<std::string> missing_from(const std::vector<std::string>& flags,
                                      const std::vector<std::string>& from) {
    std::vector<std::string> found;
    std::ranges::set_difference(flags, from, std::back_inserter(found));
    return found;
}

// Why the build ran: as recorded, else from the one of its cp and slot before it.
std::optional<BuildReason> reason_after(const BuildRecord& record,
                                        const std::optional<BuildRecord>& previous) {
    if (record.reason || !previous) {
        return record.reason;
    }
    const auto before = parse_version(split_cpv(previous->cpv).version);
    const auto now = parse_version(split_cpv(record.cpv).version);
    if (before && now) {
        if (const auto order = vercmp(*now, *before); order != 0) {
            return order > 0 ? BuildReason::upgrade : BuildReason::downgrade;
        }
    }
    return record.use == previous->use ? BuildReason::reinstall : BuildReason::use;
}

std::string without_tabs(std::string text) {
    std::ranges::replace(text, '\t', ' ');
    return text;
}

template <class Map> std::vector<std::string> keys_of(const Map& map) {
    std::vector<std::string> found;
    found.reserve(map.size());
    for (const auto& [key, value] : map) {
        found.push_back(key);
    }
    return found;
}

} // namespace

std::vector<RebuildCounts> frequent_rebuilds(std::span<const BuildRecord> records) {
    struct Counting {
        RebuildCounts counts;
        std::map<std::string, std::size_t> flags;
        std::map<std::string, std::size_t> causes;
    };
    std::map<std::string, Counting> by_cp;
    // By cp and slot, the latest build so far.
    std::map<std::pair<std::string, std::string>, BuildRecord> latest;
    for (const auto& record : records) {
        const auto cp = cp_of(record.cpv);
        auto& counting =
            by_cp.try_emplace(cp, Counting{.counts = {.cp = cp}, .flags = {}, .causes = {}})
                .first->second;
        ++counting.counts.builds;
        const auto key = std::pair{cp, record.slot};
        const auto previous = latest.find(key);
        const auto reason = reason_after(
            record, previous == latest.end() ? std::nullopt : std::optional{previous->second});
        if (reason) {
            ++counting.counts.reasons.at(static_cast<std::size_t>(*reason));
        } else {
            ++counting.counts.unknown;
        }
        if (previous != latest.end()) {
            for (const auto& flag : missing_from(record.use, previous->second.use)) {
                ++counting.flags[flag];
            }
            for (const auto& flag : missing_from(previous->second.use, record.use)) {
                ++counting.flags[flag];
            }
        }
        if (const auto& bound = record.rebuilt_for) {
            ++counting.causes[cp_of(*bound)];
        }
        latest.insert_or_assign(key, record);
    }
    std::vector<RebuildCounts> found;
    for (auto& [cp, counting] : by_cp) {
        if (counting.counts.builds < 2) {
            continue;
        }
        counting.counts.flags = tallies(counting.flags);
        counting.counts.causes = tallies(counting.causes);
        found.push_back(std::move(counting.counts));
    }
    std::ranges::stable_sort(found, std::greater{}, &RebuildCounts::builds);
    return found;
}

std::string rebuild_fields(const RebuildCounts& counts) {
    std::string reasons;
    for (const auto count : counts.reasons) {
        reasons += std::format("\t{}", count);
    }
    return std::format("{}\t{}{}\t{}\t{}\t{}", counts.cp, counts.builds, reasons, counts.unknown,
                       tally_text(counts.flags), tally_text(counts.causes));
}

std::vector<Failures> failing_packages(std::span<const BuildRecord> records) {
    struct Counting {
        Failures failures;
        std::map<std::string, std::size_t> phases;
        // The cpvs of failures no later build of theirs merged past yet.
        std::vector<std::string> unpassed;
    };
    std::map<std::string, Counting> by_cp;
    for (const auto& record : records) {
        const auto cp = cp_of(record.cpv);
        auto& counting =
            by_cp.try_emplace(cp, Counting{.failures = {.cp = cp}, .phases = {}, .unpassed = {}})
                .first->second;
        auto& failures = counting.failures;
        ++failures.builds;
        if (record.outcome == BuildRecord::Outcome::merged) {
            failures.merged_since = true;
            failures.passed_later += std::erase(counting.unpassed, record.cpv);
            continue;
        }
        ++failures.failed;
        ++counting.phases[record.failed_phase.value_or("worker")];
        counting.unpassed.push_back(record.cpv);
        failures.last_start = record.start;
        failures.last_cpv = record.cpv;
        failures.last_log = record.log.value_or(record.error.value_or(""));
        failures.merged_since = false;
    }
    std::vector<Failures> found;
    for (auto& [cp, counting] : by_cp) {
        if (counting.failures.failed == 0) {
            continue;
        }
        counting.failures.phases = tallies(counting.phases);
        found.push_back(std::move(counting.failures));
    }
    std::ranges::stable_sort(found, std::greater{}, &Failures::last_start);
    return found;
}

std::string failures_fields(const Failures& failures) {
    return std::format(
        "{}\t{}\t{}\t{}\t{}\t{:.0f}\t{}\t{}\t{}", failures.cp, failures.builds, failures.failed,
        failures.passed_later, failures.merged_since ? "merged" : "failing", failures.last_start,
        failures.last_cpv, tally_text(failures.phases), without_tabs(failures.last_log));
}

std::vector<BuildChange> build_changes(const BuildRecord& before, const BuildRecord& after) {
    std::vector<BuildChange> found;
    const auto changed = [&](std::string what, std::string old, std::string now) {
        if (old != now) {
            found.push_back(
                {.what = std::move(what), .before = std::move(old), .after = std::move(now)});
        }
    };
    changed("version", std::string{split_cpv(before.cpv).version},
            std::string{split_cpv(after.cpv).version});
    std::vector<std::string> cps;
    std::ranges::set_union(keys_of(before.toolchain), keys_of(after.toolchain),
                           std::back_inserter(cps));
    for (const auto& cp : cps) {
        const auto old = before.toolchain.find(cp);
        const auto now = after.toolchain.find(cp);
        changed(cp, old == before.toolchain.end() ? "" : joined(old->second),
                now == after.toolchain.end() ? "" : joined(now->second));
    }
    std::vector<std::string> names;
    std::ranges::set_union(keys_of(before.environment), keys_of(after.environment),
                           std::back_inserter(names));
    for (const auto& name : names) {
        const auto old = before.environment.find(name);
        const auto now = after.environment.find(name);
        changed(name, old == before.environment.end() ? "" : old->second,
                now == after.environment.end() ? "" : now->second);
    }
    changed("package.env", joined(before.env_files), joined(after.env_files));
    const auto off = missing_from(before.use, after.use);
    const auto on = missing_from(after.use, before.use);
    if (!off.empty() || !on.empty()) {
        found.push_back({.what = "USE", .before = joined(off), .after = joined(on)});
    }
    return found;
}

std::vector<Slowdown> slower_builds(std::span<const BuildRecord> records) {
    std::map<std::string, std::vector<BuildRecord>> merged;
    for (const auto& record : records) {
        if (record.outcome == BuildRecord::Outcome::merged) {
            merged[cp_of(record.cpv)].push_back(record);
        }
    }
    std::vector<Slowdown> found;
    for (const auto& [cp, builds] : merged) {
        if (builds.size() < 2) {
            continue;
        }
        const auto& latest = builds.back();
        const auto earlier = std::span{builds}
                                 .first(builds.size() - 1)
                                 .last(std::min(slower_earlier, builds.size() - 1));
        const bool cpu =
            latest.cpu_seconds && std::ranges::all_of(earlier, [](const BuildRecord& record) {
                return record.cpu_seconds.has_value();
            });
        // Unmeasured, a build beside others took only what they left it.
        if (!cpu && latest.beside > 0) {
            continue;
        }
        const auto measure = [&](const BuildRecord& record) {
            return cpu ? record.cpu_seconds.value_or(0) : seconds_building(record);
        };
        std::vector<double> before;
        for (const auto& record : earlier) {
            if (cpu || record.beside == 0) {
                before.push_back(measure(record));
            }
        }
        if (before.empty()) {
            continue;
        }
        const auto seconds = measure(latest);
        const auto typical = median(before);
        if (seconds < typical * slower_share || seconds - typical < slower_seconds) {
            continue;
        }
        found.push_back({.cp = cp,
                         .cpv = latest.cpv,
                         .start = latest.start,
                         .cpu = cpu,
                         .seconds = seconds,
                         .before = typical,
                         .changes = build_changes(builds.at(builds.size() - 2), latest)});
    }
    std::ranges::stable_sort(found, std::greater{}, [](const Slowdown& slowdown) {
        return slowdown.seconds / slowdown.before;
    });
    return found;
}

std::vector<std::string> slowdown_lines(const Slowdown& slowdown) {
    std::vector<std::string> lines{
        std::format("{}\t{}\t{:.0f}\t{}\t{:.1f}\t{:.1f}", slowdown.cp, slowdown.cpv, slowdown.start,
                    slowdown.cpu ? "cpu" : "wall", slowdown.seconds, slowdown.before)};
    for (const auto& change : slowdown.changes) {
        lines.push_back(std::format("\tchange\t{}\t{}\t{}", change.what,
                                    without_tabs(change.before), without_tabs(change.after)));
    }
    return lines;
}

} // namespace egraph
