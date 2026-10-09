#include "emerge_builds.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <system_error>
#include <utility>

namespace egraph {

namespace {

// Seconds after a build starts within which emerge has published its first phase: at most a
// second between writes, and every two seconds regardless.
constexpr double first_phase_seen = 2.5;

// A build in a snapshot: built from source, to be merged.
bool is_build(const emerge::Task& task) {
    return !task.binary && (task.operation.empty() || task.operation == "merge");
}

// Still building: neither waiting to merge nor merging.
bool still_building(const emerge::Task& task) {
    return task.kind == emerge::TaskKind::build && !task.merge_wait && task.phase != "merge-wait";
}

void end_phase(EmergeWatch::Following& following, double at) {
    if (following.phase) {
        following.build.phases.push_back({.phase = std::move(*following.phase),
                                          .seconds = std::max(at - following.phase_start, 0.0)});
        following.phase.reset();
    }
}

std::optional<std::string> read_line(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::string line;
    if (!in || !std::getline(in, line)) {
        return std::nullopt;
    }
    return line;
}

std::set<std::string> words(std::string_view text) {
    std::set<std::string> found;
    for (const auto word : std::views::split(text, ' ')) {
        if (!word.empty()) {
            found.emplace(word.begin(), word.end());
        }
    }
    return found;
}

template <class T> std::optional<T> number(const std::optional<std::string>& line) {
    if (!line) {
        return std::nullopt;
    }
    const std::string_view text{*line};
    T value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::vector<EmergeBuild> EmergeWatch::observe(const std::vector<emerge::Snapshot>& live,
                                              double now) {
    std::set<std::pair<std::int64_t, std::string>> seen;
    std::set<std::int64_t> running;
    for (const auto& snapshot : live) {
        running.insert(snapshot.pid);
        const auto builds_now = static_cast<std::uint32_t>(
            std::ranges::count_if(snapshot.tasks, [](const emerge::Task& task) {
                return is_build(task) && still_building(task);
            }));
        for (const auto& task : snapshot.tasks) {
            if (!is_build(task)) {
                continue;
            }
            const auto key = std::pair{snapshot.pid, task.cpv};
            seen.insert(key);
            auto [found, added] = builds_.try_emplace(key);
            auto& following = found->second;
            auto& build = following.build;
            if (added) {
                build.pid = snapshot.pid;
                build.cpv = task.cpv;
                build.root = task.root;
                build.start = task.start_time.value_or(snapshot.timestamp);
                build.jobs = snapshot.jobs.max;
            }
            if (task.resources != emerge::Resources{}) {
                build.resources = task.resources;
            }
            if (still_building(task)) {
                build.beside = std::max(build.beside, builds_now - 1);
                if (!task.phase.empty() && following.phase != task.phase) {
                    // The first phase, seen as soon as emerge publishes it, started with the
                    // build; one seen later only when seen, the time before it unaccounted.
                    const bool first = !following.phase && build.phases.empty() &&
                                       snapshot.timestamp - build.start <= first_phase_seen;
                    end_phase(following, snapshot.timestamp);
                    following.phase = task.phase;
                    following.phase_start = first ? build.start : snapshot.timestamp;
                }
                continue;
            }
            if (!build.built) {
                build.built =
                    task.build_elapsed ? build.start + *task.build_elapsed : snapshot.timestamp;
                end_phase(following, *build.built);
            }
            if (task.kind == emerge::TaskKind::build) {
                following.waited = true;
            } else if (!build.merge_start) {
                build.merging = true;
                build.merge_start = following.waited ? snapshot.timestamp : *build.built;
            }
        }
    }
    std::vector<EmergeBuild> gone;
    for (auto at = builds_.begin(); at != builds_.end();) {
        if (seen.contains(at->first)) {
            ++at;
            continue;
        }
        auto& following = at->second;
        end_phase(following, now);
        following.build.ended = now;
        following.build.emerge_running = running.contains(at->first.first);
        gone.push_back(std::move(following.build));
        at = builds_.erase(at);
    }
    return gone;
}

std::optional<VdbEntry> read_vdb_entry(const std::filesystem::path& vdb, const std::string& cpv) {
    const auto entry = vdb / std::filesystem::path{cpv};
    std::error_code error;
    if (!std::filesystem::is_directory(entry, error)) {
        return std::nullopt;
    }
    VdbEntry found;
    found.repo = read_line(entry / "repository").value_or("");
    const auto slot = read_line(entry / "SLOT").value_or("");
    found.slot = slot.substr(0, slot.find('/'));
    std::set<std::string> iuse;
    for (const auto& flag : words(read_line(entry / "IUSE").value_or(""))) {
        iuse.insert(flag.starts_with('+') || flag.starts_with('-') ? flag.substr(1) : flag);
    }
    for (const auto& flag : words(read_line(entry / "USE").value_or(""))) {
        if (iuse.contains(flag)) {
            found.use.push_back(flag);
        }
    }
    for (const auto key : build_environment_keys) {
        if (auto value = read_line(entry / key)) {
            found.environment.emplace(key, std::move(*value));
        }
    }
    if (const auto built = number<std::int64_t>(read_line(entry / "BUILD_TIME"))) {
        found.build_time = static_cast<double>(*built);
    }
    found.size = number<std::uint64_t>(read_line(entry / "SIZE"));
    return found;
}

std::optional<BuildRecord> emerge_record(const EmergeBuild& build,
                                         const std::optional<VdbEntry>& entry,
                                         const Toolchain& toolchain) {
    // BUILD_TIME is when the image was made, after the build started.
    const bool merged = entry && entry->build_time && *entry->build_time >= build.start;
    if (!merged && !build.emerge_running) {
        return std::nullopt;
    }
    BuildRecord record{.origin = "emerge",
                       .run = std::format("emerge-{}", build.pid),
                       .start = build.start,
                       .cpv = build.cpv,
                       .phases = build.phases,
                       .build_seconds = build.built.value_or(build.ended) - build.start,
                       .outcome =
                           merged ? BuildRecord::Outcome::merged : BuildRecord::Outcome::failed,
                       .beside = build.beside};
    for (const auto& [cp, slots] : toolchain) {
        auto& versions = record.toolchain.try_emplace(cp).first->second;
        for (const auto& version : slots | std::views::values) {
            versions.push_back(version);
        }
    }
    if (build.jobs && std::in_range<std::uint32_t>(*build.jobs)) {
        record.jobs = static_cast<std::uint32_t>(*build.jobs);
    }
    if (merged) {
        record.repo = entry->repo;
        record.slot = entry->slot;
        record.use = entry->use;
        record.environment = entry->environment;
        record.installed_size = entry->size;
        if (build.merge_start) {
            record.merge_seconds = build.ended - *build.merge_start;
            if (build.built) {
                record.waited = *build.merge_start - *build.built;
            }
        }
    } else if (build.merging) {
        record.failed_phase = "merge";
    } else if (!build.phases.empty()) {
        record.failed_phase = build.phases.back().phase;
    }
    const auto& resources = build.resources;
    if (resources.cpu_usec) {
        record.cpu_seconds = static_cast<double>(*resources.cpu_usec) / 1e6;
    }
    record.peak_memory = resources.mem_peak;
    record.read_bytes = resources.io_read_bytes;
    record.write_bytes = resources.io_write_bytes;
    if (resources != emerge::Resources{}) {
        record.measured = Measured::cgroup;
    }
    return record;
}

} // namespace egraph
