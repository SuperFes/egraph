#include "emerge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace egraph::emerge {

namespace {

using Json = nlohmann::json;

// A member's value when it has type T, as json's is_* functions tell; json's operator[] would
// throw or insert instead.
template <class T>
std::optional<T> value_of(const Json& object, std::string_view key, bool (Json::*is)() const) {
    const auto found = object.find(key);
    if (found == object.end() || !((*found).*is)()) {
        return std::nullopt;
    }
    return found->get<T>();
}

std::optional<std::uint64_t> unsigned_of(const Json& object, std::string_view key) {
    return value_of<std::uint64_t>(object, key, &Json::is_number_unsigned);
}

std::optional<std::int64_t> integer_of(const Json& object, std::string_view key) {
    return value_of<std::int64_t>(object, key, &Json::is_number_integer);
}

std::optional<double> number_of(const Json& object, std::string_view key) {
    return value_of<double>(object, key, &Json::is_number);
}

std::string string_of(const Json& object, std::string_view key) {
    return value_of<std::string>(object, key, &Json::is_string).value_or("");
}

bool bool_of(const Json& object, std::string_view key) {
    return value_of<bool>(object, key, &Json::is_boolean).value_or(false);
}

// A member that is an object, or an empty one.
Json object_of(const Json& object, std::string_view key) {
    return value_of<Json>(object, key, &Json::is_object).value_or(Json::object());
}

Resources resources_of(const Json& task) {
    const auto object = object_of(task, "resources");
    return {.cpu_usec = unsigned_of(object, "cpu_usec"),
            .mem_current = unsigned_of(object, "mem_current"),
            .mem_peak = unsigned_of(object, "mem_peak"),
            .io_read_bytes = unsigned_of(object, "io_read_bytes"),
            .io_write_bytes = unsigned_of(object, "io_write_bytes")};
}

Task task_of(const Json& object) {
    return {.cpv = string_of(object, "cpv"),
            .kind = string_of(object, "kind") == "merge" ? TaskKind::merge : TaskKind::build,
            .phase = string_of(object, "phase"),
            .binary = bool_of(object, "binary"),
            .merge_wait = bool_of(object, "merge_wait"),
            .pid = integer_of(object, "pid"),
            .elapsed = number_of(object, "elapsed"),
            .build_elapsed = number_of(object, "build_elapsed"),
            .resources = resources_of(object)};
}

Jobs jobs_of(const Json& snapshot) {
    const auto object = object_of(snapshot, "jobs");
    return {.running = unsigned_of(object, "running").value_or(0),
            .max = unsigned_of(object, "max"),
            .completed = unsigned_of(object, "completed").value_or(0),
            .total = unsigned_of(object, "total").value_or(0),
            .failed = unsigned_of(object, "failed").value_or(0),
            .merge_wait = unsigned_of(object, "merge_wait").value_or(0),
            .merges_pending = unsigned_of(object, "merges_pending").value_or(0)};
}

// The pid in an emerge-<pid>.json name.
std::optional<std::int64_t> named_pid(const std::filesystem::path& path) {
    if (path.extension() != ".json") {
        return std::nullopt;
    }
    const auto stem = path.stem().string();
    constexpr std::string_view prefix = "emerge-";
    if (!stem.starts_with(prefix) || stem.size() == prefix.size()) {
        return std::nullopt;
    }
    const std::string_view digits = std::string_view{stem}.substr(prefix.size());
    std::int64_t pid = 0;
    const auto [end, error] = std::from_chars(digits.begin(), digits.end(), pid);
    if (error != std::errc{} || end != digits.end() || pid <= 0) {
        return std::nullopt;
    }
    return pid;
}

} // namespace

std::expected<Snapshot, std::string> parse_snapshot(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    if (string_of(json, "type") != "snapshot") {
        return std::unexpected("not a snapshot");
    }
    if (const auto schema = unsigned_of(json, "schema"); schema != 1) {
        return std::unexpected("not a schema 1 snapshot");
    }
    const auto pid = integer_of(json, "emerge_pid");
    if (!pid) {
        return std::unexpected("no emerge_pid");
    }
    Snapshot snapshot{.pid = *pid,
                      .timestamp = number_of(json, "timestamp").value_or(0),
                      .jobs = jobs_of(json),
                      .tasks = {}};
    if (const auto tasks = value_of<Json>(json, "tasks", &Json::is_array)) {
        for (const auto& task : *tasks) {
            if (task.is_object()) {
                snapshot.tasks.push_back(task_of(task));
            }
        }
    }
    return snapshot;
}

std::filesystem::path status_dir(const std::filesystem::path& eprefix) {
    return std::filesystem::path{"/"} / eprefix.relative_path() / "run/portage";
}

std::vector<Snapshot> read_snapshots(const std::filesystem::path& dir,
                                     const std::filesystem::path& proc) {
    std::vector<Snapshot> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{dir, error}) {
        const auto pid = named_pid(entry.path());
        if (!pid || !std::filesystem::exists(proc / std::to_string(*pid), error)) {
            continue;
        }
        std::ifstream in{entry.path()};
        std::ostringstream text;
        text << in.rdbuf();
        auto snapshot = parse_snapshot(text.str());
        if (snapshot && snapshot->pid == *pid) {
            found.push_back(std::move(*snapshot));
        }
    }
    std::ranges::sort(found, {}, &Snapshot::pid);
    return found;
}

} // namespace egraph::emerge
