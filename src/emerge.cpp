#include "emerge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <ranges>
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
            .root = string_of(object, "root"),
            .operation = string_of(object, "operation"),
            .kind = string_of(object, "kind") == "merge" ? TaskKind::merge : TaskKind::build,
            .phase = string_of(object, "phase"),
            .binary = bool_of(object, "binary"),
            .merge_wait = bool_of(object, "merge_wait"),
            .pid = integer_of(object, "pid"),
            .start_time = number_of(object, "start_time"),
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

std::string_view name_prefix(Publisher publisher) {
    return publisher == Publisher::exec ? "exec-" : "emerge-";
}

// The pid in an emerge-<pid>.json (or exec-<pid>.json) name.
std::optional<std::int64_t> named_pid(const std::filesystem::path& path, std::string_view prefix) {
    if (path.extension() != ".json") {
        return std::nullopt;
    }
    const auto stem = path.stem().string();
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

std::string snapshot_json(const Snapshot& snapshot) {
    // Unset as null, as Python's None.
    const auto or_null = [](const auto& value) { return value ? Json(*value) : Json(nullptr); };
    Json tasks = Json::array();
    for (const auto& task : snapshot.tasks) {
        const auto slash = std::min(task.cpv.find('/'), task.cpv.size());
        Json entry{{"cpv", task.cpv},
                   {"category", task.cpv.substr(0, slash)},
                   {"pf", task.cpv.substr(std::min(slash + 1, task.cpv.size()))},
                   {"root", task.root},
                   {"operation", task.operation},
                   {"binary", task.binary},
                   {"kind", task.kind == TaskKind::merge ? "merge" : "build"},
                   {"phase", task.phase.empty() ? Json(nullptr) : Json(task.phase)},
                   {"merge_wait", task.merge_wait},
                   {"pid", or_null(task.pid)},
                   {"start_time", or_null(task.start_time)},
                   {"elapsed", or_null(task.elapsed)},
                   {"build_elapsed", or_null(task.build_elapsed)}};
        Json resources = Json::object();
        for (const auto& [key, value] :
             {std::pair{"cpu_usec", task.resources.cpu_usec},
              std::pair{"mem_current", task.resources.mem_current},
              std::pair{"mem_peak", task.resources.mem_peak},
              std::pair{"io_read_bytes", task.resources.io_read_bytes},
              std::pair{"io_write_bytes", task.resources.io_write_bytes}}) {
            if (value) {
                resources.emplace(key, *value);
            }
        }
        if (!resources.empty()) {
            entry.emplace("resources", std::move(resources));
        }
        tasks.push_back(std::move(entry));
    }
    const auto& jobs = snapshot.jobs;
    // emerge's _max_jobs is True for --jobs without a limit.
    const Json json{{"type", "snapshot"},
                    {"schema", 1},
                    {"emerge_pid", snapshot.pid},
                    {"timestamp", snapshot.timestamp},
                    {"jobs",
                     {{"running", jobs.running},
                      {"max", jobs.max ? Json(*jobs.max) : Json(true)},
                      {"completed", jobs.completed},
                      {"total", jobs.total},
                      {"failed", jobs.failed},
                      {"merge_wait", jobs.merge_wait},
                      {"merges_pending", jobs.merges_pending}}},
                    {"tasks", std::move(tasks)}};
    return json.dump();
}

std::filesystem::path status_dir(const std::filesystem::path& eprefix, Publisher publisher) {
    return std::filesystem::path{"/"} / eprefix.relative_path() /
           (publisher == Publisher::exec ? "run/egraph" : "run/portage");
}

std::vector<Snapshot> read_snapshots(const std::filesystem::path& dir,
                                     const std::filesystem::path& proc, Publisher publisher) {
    std::vector<Snapshot> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{dir, error}) {
        const auto pid = named_pid(entry.path(), name_prefix(publisher));
        if (!pid || !std::filesystem::exists(proc / std::to_string(*pid), error)) {
            continue;
        }
        std::ifstream in{entry.path()};
        std::ostringstream text;
        text << in.rdbuf();
        auto snapshot = parse_snapshot(text.str());
        if (snapshot && snapshot->pid == *pid) {
            snapshot->publisher = publisher;
            found.push_back(std::move(*snapshot));
        }
    }
    std::ranges::sort(found, {}, &Snapshot::pid);
    return found;
}

std::filesystem::path mtimedb_path(const std::filesystem::path& eprefix) {
    return std::filesystem::path{"/"} / eprefix.relative_path() / "var/cache/edb/mtimedb";
}

std::vector<Pending> parse_mergelist(std::string_view mtimedb) {
    const auto json = Json::parse(mtimedb, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return {};
    }
    const auto list = value_of<Json>(object_of(json, "resume"), "mergelist", &Json::is_array);
    if (!list) {
        return {};
    }
    std::vector<Pending> pending;
    for (const auto& entry : *list) {
        // [kind, root, cpv, operation], as a Package iterates.
        if (!entry.is_array() || entry.size() < 3 ||
            !std::ranges::all_of(entry, &Json::is_string)) {
            continue;
        }
        pending.push_back({.kind = entry.at(0).get<std::string>(),
                           .root = entry.at(1).get<std::string>(),
                           .cpv = entry.at(2).get<std::string>()});
    }
    return pending;
}

std::expected<Waits, std::string> parse_waits(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    Waits waits;
    for (const auto& [cpv, list] : json.items()) {
        if (!list.is_array() || !std::ranges::all_of(list, &Json::is_string)) {
            return std::unexpected(std::format("{}: not a list of cpvs", cpv));
        }
        auto& found = waits[cpv];
        for (const auto& other : list) {
            found.push_back(other.get<std::string>());
        }
    }
    return waits;
}

std::vector<Branch> hierarchy(const std::vector<Pending>& pending, const Waits& waits) {
    std::map<std::string_view, std::size_t, std::less<>> position;
    for (std::size_t at = 0; at < pending.size(); ++at) {
        position.emplace(pending.at(at).cpv, at);
    }
    // Each package's parent: what it waits for that merges last before it.
    std::vector<std::optional<std::size_t>> parent(pending.size());
    std::vector<std::size_t> waiting_for(pending.size(), 0);
    std::vector<std::vector<std::size_t>> children(pending.size());
    std::vector<std::size_t> top;
    for (std::size_t at = 0; at < pending.size(); ++at) {
        if (const auto found = waits.find(pending.at(at).cpv); found != waits.end()) {
            for (const auto& other : found->second) {
                const auto where = position.find(other);
                if (where == position.end() || where->second == at) {
                    continue;
                }
                ++waiting_for.at(at);
                auto& chosen = parent.at(at);
                if (where->second < at && where->second >= chosen.value_or(0)) {
                    chosen = where->second;
                }
            }
        }
        if (const auto& chosen = parent.at(at)) {
            children.at(*chosen).push_back(at);
        } else {
            top.push_back(at);
        }
    }
    // Depth first, in merge order; parents come before their children, so a stack will do.
    std::vector<Branch> tree;
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    for (const auto root : std::ranges::reverse_view(top)) {
        stack.emplace_back(root, 1);
    }
    while (!stack.empty()) {
        const auto [at, depth] = stack.back();
        stack.pop_back();
        tree.push_back(
            {.cpv = pending.at(at).cpv, .depth = depth, .waiting_for = waiting_for.at(at)});
        for (const auto child : std::ranges::reverse_view(children.at(at))) {
            stack.emplace_back(child, depth + 1);
        }
    }
    // Tree lines, walking up from the bottom as tui::thread does.
    std::vector<bool> later;
    for (auto& branch : std::ranges::reverse_view(tree)) {
        const auto depth = branch.depth;
        later.resize(std::max(later.size(), depth + 1), false);
        branch.last = !later.at(depth);
        branch.rails.assign(depth - 1, false);
        for (std::size_t level = 1; level < depth; ++level) {
            branch.rails.at(level - 1) = later.at(level);
        }
        later.at(depth) = true;
        std::fill(later.begin() + static_cast<std::ptrdiff_t>(depth) + 1, later.end(), false);
    }
    return tree;
}

} // namespace egraph::emerge
