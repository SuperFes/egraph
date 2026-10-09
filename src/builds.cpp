#include "builds.hpp"

#include "atom.hpp"
#include "human.hpp"
#include "log.hpp"
#include "log_read.hpp"
#include "os.hpp"
#include "use_stack.hpp"
#include "version.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <utility>

namespace egraph {

namespace {

using Json = nlohmann::json;

std::optional<std::string> string_at(const Json& json, std::string_view key) {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

std::optional<double> number_at(const Json& json, std::string_view key) {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_number()) {
        return std::nullopt;
    }
    return found->get<double>();
}

std::optional<std::int64_t> integer_at(const Json& json, std::string_view key) {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    return found->get<std::int64_t>();
}

std::optional<std::uint64_t> unsigned_at(const Json& json, std::string_view key) {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

// The strings of an array, none for anything else.
std::optional<std::vector<std::string>> strings(const Json& json) {
    if (!json.is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> values;
    values.reserve(json.size());
    for (const auto& each : json) {
        if (!each.is_string()) {
            return std::nullopt;
        }
        values.push_back(each.get<std::string>());
    }
    return values;
}

// What the record has under key: an array of strings, empty when it has none.
std::expected<std::vector<std::string>, std::string> strings_at(const Json& json,
                                                                std::string_view key) {
    const auto found = json.find(key);
    if (found == json.end()) {
        return std::vector<std::string>{};
    }
    auto values = strings(*found);
    if (!values) {
        return std::unexpected(std::format("{} is not a list of strings", key));
    }
    return std::move(*values);
}

std::string_view outcome_name(BuildRecord::Outcome outcome) {
    return outcome == BuildRecord::Outcome::merged ? "merged" : "failed";
}

std::string_view measured_name(Measured measured) {
    return measured == Measured::cgroup ? "cgroup" : "proc";
}

// The record's cp, if the line is one.
std::optional<std::string> record_cp(std::string_view line) {
    const auto record = parse_build_record(line);
    if (!record) {
        return std::nullopt;
    }
    const auto parts = split_cpv(record->cpv);
    return std::format("{}/{}", parts.category, parts.name);
}

} // namespace

std::filesystem::path builds_path(const std::filesystem::path& eroot) {
    return eroot / "var/lib/egraph/builds.jsonl";
}

std::string build_record_json(const BuildRecord& record) {
    auto phases = Json::array();
    for (const auto& phase : record.phases) {
        Json each{{"phase", phase.phase}, {"seconds", phase.seconds}};
        if (phase.cpu_seconds) {
            each.emplace("cpu_seconds", *phase.cpu_seconds);
        }
        phases.push_back(std::move(each));
    }
    Json json{{"version", build_record_version},
              {"origin", record.origin},
              {"run", record.run},
              {"start", record.start},
              {"cpv", record.cpv},
              {"repo", record.repo},
              {"slot", record.slot},
              {"use", record.use},
              {"env_files", record.env_files},
              {"environment", record.environment},
              {"toolchain", record.toolchain},
              {"phases", std::move(phases)},
              {"build_seconds", record.build_seconds},
              {"outcome", outcome_name(record.outcome)},
              {"beside", record.beside}};
    if (record.merge_seconds) {
        json.emplace("merge_seconds", *record.merge_seconds);
    }
    if (record.waited) {
        json.emplace("waited", *record.waited);
    }
    if (record.failed_phase) {
        json.emplace("failed_phase", *record.failed_phase);
    }
    if (record.status) {
        json.emplace("status", *record.status);
    }
    if (record.log) {
        json.emplace("log", *record.log);
    }
    if (record.error) {
        json.emplace("error", *record.error);
    }
    if (record.jobs) {
        json.emplace("jobs", *record.jobs);
    }
    if (record.cpu_seconds) {
        json.emplace("cpu_seconds", *record.cpu_seconds);
    }
    if (record.peak_memory) {
        json.emplace("peak_memory", *record.peak_memory);
    }
    if (record.read_bytes) {
        json.emplace("read_bytes", *record.read_bytes);
    }
    if (record.write_bytes) {
        json.emplace("write_bytes", *record.write_bytes);
    }
    if (record.measured) {
        json.emplace("measured", measured_name(*record.measured));
    }
    if (record.build_stall) {
        auto stall = Json::object();
        const auto share = [&](std::string_view key, std::optional<double> value) {
            if (value) {
                stall.emplace(key, *value);
            }
        };
        share("cpu", record.build_stall->cpu);
        share("memory", record.build_stall->memory);
        share("io", record.build_stall->io);
        json.emplace("build_stall", std::move(stall));
    }
    const auto size = [&](std::string_view key, std::optional<std::uint64_t> value) {
        if (value) {
            json.emplace(key, *value);
        }
    };
    size("build_dir_size", record.build_dir_size);
    size("image_size", record.image_size);
    size("distfiles_size", record.distfiles_size);
    size("installed_size", record.installed_size);
    if (record.jobserver) {
        json.emplace("jobserver",
                     Json{{"jobs", record.jobserver->jobs}, {"free", record.jobserver->free}});
    }
    if (record.system) {
        auto system = Json::object();
        const auto share = [&](std::string_view key, std::optional<double> value) {
            if (value) {
                system.emplace(key, *value);
            }
        };
        share("cpu_busy", record.system->cpu_busy);
        share("load", record.system->load);
        share("stall_cpu", record.system->stall_cpu);
        share("stall_memory", record.system->stall_memory);
        share("stall_io", record.system->stall_io);
        json.emplace("system", std::move(system));
    }
    return json.dump();
}

std::expected<BuildRecord, std::string> parse_build_record(std::string_view line) {
    const auto json = Json::parse(line, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    if (integer_at(json, "version") != build_record_version) {
        return std::unexpected("not a build record of this version");
    }
    BuildRecord record;
    auto origin = string_at(json, "origin");
    auto run = string_at(json, "run");
    const auto start = number_at(json, "start");
    auto cpv = string_at(json, "cpv");
    auto repo = string_at(json, "repo");
    auto slot = string_at(json, "slot");
    const auto build_seconds = number_at(json, "build_seconds");
    const auto outcome = string_at(json, "outcome");
    if (!origin || !run || !start || !cpv || !repo || !slot || !build_seconds || !outcome) {
        return std::unexpected("a field it needs is missing");
    }
    if (*outcome != "merged" && *outcome != "failed") {
        return std::unexpected("an unknown outcome: " + *outcome);
    }
    record.origin = std::move(*origin);
    record.run = std::move(*run);
    record.start = *start;
    record.cpv = std::move(*cpv);
    record.repo = std::move(*repo);
    record.slot = std::move(*slot);
    record.build_seconds = *build_seconds;
    record.outcome =
        *outcome == "merged" ? BuildRecord::Outcome::merged : BuildRecord::Outcome::failed;
    auto use = strings_at(json, "use");
    auto env_files = strings_at(json, "env_files");
    if (!use || !env_files) {
        return std::unexpected(use ? env_files.error() : use.error());
    }
    record.use = std::move(*use);
    record.env_files = std::move(*env_files);
    if (const auto found = json.find("environment"); found != json.end()) {
        if (!found->is_object()) {
            return std::unexpected("environment is not an object");
        }
        for (const auto& [name, value] : found->items()) {
            if (!value.is_string()) {
                return std::unexpected("environment holds a value that is not a string");
            }
            record.environment.emplace(name, value.get<std::string>());
        }
    }
    if (const auto found = json.find("toolchain"); found != json.end()) {
        if (!found->is_object()) {
            return std::unexpected("toolchain is not an object");
        }
        for (const auto& [cp, value] : found->items()) {
            auto versions = strings(value);
            if (!versions) {
                return std::unexpected("toolchain holds a value that is not a list of strings");
            }
            record.toolchain.emplace(cp, std::move(*versions));
        }
    }
    if (const auto found = json.find("phases"); found != json.end()) {
        if (!found->is_array()) {
            return std::unexpected("phases is not a list");
        }
        for (const auto& each : *found) {
            auto phase = each.is_object() ? string_at(each, "phase") : std::nullopt;
            const auto seconds = each.is_object() ? number_at(each, "seconds") : std::nullopt;
            if (!phase || !seconds) {
                return std::unexpected("a phase without its name or seconds");
            }
            record.phases.push_back({.phase = std::move(*phase),
                                     .seconds = *seconds,
                                     .cpu_seconds = number_at(each, "cpu_seconds")});
        }
    }
    record.merge_seconds = number_at(json, "merge_seconds");
    record.waited = number_at(json, "waited");
    record.failed_phase = string_at(json, "failed_phase");
    record.status = integer_at(json, "status");
    record.log = string_at(json, "log");
    record.error = string_at(json, "error");
    if (const auto jobs = integer_at(json, "jobs")) {
        if (!std::in_range<std::uint32_t>(*jobs)) {
            return std::unexpected("jobs out of range");
        }
        record.jobs = static_cast<std::uint32_t>(*jobs);
    }
    const auto beside = integer_at(json, "beside");
    if (beside && !std::in_range<std::uint32_t>(*beside)) {
        return std::unexpected("beside out of range");
    }
    record.beside = beside ? static_cast<std::uint32_t>(*beside) : 0;
    record.cpu_seconds = number_at(json, "cpu_seconds");
    record.peak_memory = unsigned_at(json, "peak_memory");
    record.read_bytes = unsigned_at(json, "read_bytes");
    record.write_bytes = unsigned_at(json, "write_bytes");
    if (const auto found = json.find("system"); found != json.end()) {
        if (!found->is_object()) {
            return std::unexpected("system is not an object");
        }
        record.system = SystemShare{.cpu_busy = number_at(*found, "cpu_busy"),
                                    .load = number_at(*found, "load"),
                                    .stall_cpu = number_at(*found, "stall_cpu"),
                                    .stall_memory = number_at(*found, "stall_memory"),
                                    .stall_io = number_at(*found, "stall_io")};
    }
    if (const auto measured = string_at(json, "measured")) {
        if (*measured != "proc" && *measured != "cgroup") {
            return std::unexpected("an unknown measure: " + *measured);
        }
        record.measured = *measured == "cgroup" ? Measured::cgroup : Measured::proc;
    }
    if (const auto found = json.find("build_stall"); found != json.end()) {
        if (!found->is_object()) {
            return std::unexpected("build_stall is not an object");
        }
        record.build_stall = StallShare{.cpu = number_at(*found, "cpu"),
                                        .memory = number_at(*found, "memory"),
                                        .io = number_at(*found, "io")};
    }
    record.build_dir_size = unsigned_at(json, "build_dir_size");
    record.image_size = unsigned_at(json, "image_size");
    record.distfiles_size = unsigned_at(json, "distfiles_size");
    record.installed_size = unsigned_at(json, "installed_size");
    if (const auto found = json.find("jobserver"); found != json.end()) {
        const auto jobs = found->is_object() ? number_at(*found, "jobs") : std::nullopt;
        const auto free = found->is_object() ? number_at(*found, "free") : std::nullopt;
        if (!jobs || !free) {
            return std::unexpected("jobserver is not jobs and free tokens");
        }
        record.jobserver = JobserverShare{.jobs = *jobs, .free = *free};
    }
    return record;
}

std::vector<BuildRecord> read_builds(std::string_view text) {
    std::vector<BuildRecord> records;
    for (const auto line : std::views::split(text, '\n')) {
        if (auto record = parse_build_record(std::string_view{line.begin(), line.end()})) {
            records.push_back(std::move(*record));
        }
    }
    return records;
}

std::string pruned_builds(std::string_view text, std::size_t keep,
                          std::optional<std::string_view> line) {
    std::vector<std::string_view> lines;
    for (const auto each : std::views::split(text, '\n')) {
        if (!each.empty()) {
            lines.emplace_back(each.begin(), each.end());
        }
    }
    if (line) {
        lines.push_back(*line);
    }
    // Newest first, so each cp's count says how many later builds of it are kept.
    std::vector<bool> kept(lines.size(), true);
    std::map<std::string, std::size_t> counts;
    for (std::size_t i = lines.size(); i-- > 0;) {
        if (const auto cp = record_cp(lines.at(i))) {
            kept.at(i) = counts.try_emplace(*cp, 0).first->second++ < keep;
        }
    }
    std::string pruned;
    for (const auto& [each, keeps] : std::views::zip(lines, kept)) {
        if (keeps) {
            pruned += each;
            pruned += '\n';
        }
    }
    return pruned;
}

std::expected<void, std::error_code> append_build(const std::filesystem::path& path,
                                                  const BuildRecord& record, std::size_t keep) {
    if (keep == 0) {
        return {};
    }
    const auto line = build_record_json(record);
    return os::rewrite_locked(
        path, [&](std::string_view whole) { return pruned_builds(whole, keep, line); });
}

std::vector<BuildStep> build_steps(const Store& store, const Evaluated& original, const Plan& plan,
                                   std::span<const Step> steps) {
    const auto& evaluated = plan.evaluated_or(original);
    const UseStacker stacker{store, evaluated};
    std::vector<BuildStep> found;
    found.reserve(steps.size());
    for (const auto& step : steps) {
        if (const auto* merge = std::get_if<MergeStep>(&step)) {
            const auto& candidate = evaluated.candidates.at(plan.merges.at(merge->merge).candidate);
            const std::string cpv{evaluated.string(candidate.cpv)};
            std::vector<std::string> use;
            for (const auto id : evaluated.ids_in(candidate.use)) {
                use.emplace_back(evaluated.string(id));
            }
            std::ranges::sort(use);
            found.push_back({.cpv = cpv,
                             .cp = std::string{evaluated.string(candidate.cp)},
                             .version = std::string{split_cpv(cpv).version},
                             .repo = std::string{evaluated.string(candidate.repo)},
                             .slot = std::string{evaluated.string(candidate.slot)},
                             .use = std::move(use),
                             .env_files = stacker.env_files(candidate),
                             .uninstall = false});
        } else {
            const auto& pkg = store.packages.at(
                plan.uninstalls.at(std::get<UninstallStep>(step).uninstall).package);
            const std::string cpv{store.string(pkg.cpv)};
            found.push_back({.cpv = cpv,
                             .cp = std::string{store.string(pkg.cp)},
                             .version = std::string{split_cpv(cpv).version},
                             .repo = std::string{store.string(pkg.repo)},
                             .slot = std::string{store.string(pkg.slot)},
                             .use = {},
                             .env_files = {},
                             .uninstall = true});
        }
    }
    return found;
}

std::optional<std::uint64_t> installed_size(const std::filesystem::path& vdb,
                                            std::string_view cpv) {
    std::ifstream in{vdb / std::filesystem::path{std::string{cpv}} / "SIZE"};
    std::uint64_t size = 0;
    if (!(in >> size)) {
        return std::nullopt;
    }
    return size;
}

Toolchain installed_toolchain(const Store& store) {
    Toolchain found;
    for (const auto& pkg : store.packages) {
        const auto cp = store.string(pkg.cp);
        if (std::ranges::contains(toolchain_cps, cp)) {
            found.try_emplace(std::string{cp})
                .first->second.insert_or_assign(
                    std::string{store.string(pkg.slot)},
                    std::string{split_cpv(store.string(pkg.cpv)).version});
        }
    }
    return found;
}

std::expected<std::vector<BuildRecord>, std::string>
matching_builds(std::vector<BuildRecord> records, std::span<const std::string> packages) {
    if (packages.empty()) {
        return records;
    }
    std::vector<Atom> atoms;
    atoms.reserve(packages.size());
    for (const auto& package : packages) {
        auto atom = parse_atom(package);
        if (!atom) {
            return std::unexpected(std::format("{}: {}", package, atom.error()));
        }
        atoms.push_back(std::move(*atom));
    }
    std::erase_if(records, [&](const BuildRecord& record) {
        const auto parts = split_cpv(record.cpv);
        const auto version = parse_version(parts.version);
        if (!version) {
            return true;
        }
        const auto cp = std::format("{}/{}", parts.category, parts.name);
        return std::ranges::none_of(atoms, [&](const Atom& atom) {
            return matches(atom, cp, *version, record.slot, "", record.repo);
        });
    });
    return records;
}

std::string build_line(const BuildRecord& record, const std::chrono::time_zone& zone) {
    auto line =
        std::format("{}  {}::{}  ", log::local_time(record.start, zone), record.cpv, record.repo);
    if (record.outcome == BuildRecord::Outcome::merged) {
        line += "merged in " + log::duration(record.build_seconds);
        if (record.merge_seconds) {
            line += ", merge " + log::duration(*record.merge_seconds);
        }
        if (record.cpu_seconds) {
            line += "; CPU " + log::duration(*record.cpu_seconds);
            if (record.peak_memory) {
                line += ", memory " + byte_size(*record.peak_memory);
            }
        } else if (record.peak_memory) {
            line += "; memory " + byte_size(*record.peak_memory);
        }
        return line;
    }
    if (record.failed_phase) {
        line += std::format("failed in {}", *record.failed_phase);
    } else {
        line += "failed";
    }
    line += " after " + log::duration(record.build_seconds);
    if (record.error) {
        line += ": " + *record.error;
    }
    return line;
}

std::string build_fields(const BuildRecord& record) {
    return std::format(
        "{:.0f}\t{}\t{}\t{}\t{:.1f}\t{}\t{}\t{}\t{}\t{}\t{}", record.start, record.cpv, record.repo,
        outcome_name(record.outcome), record.build_seconds,
        record.merge_seconds ? std::format("{:.1f}", *record.merge_seconds) : std::string{},
        record.failed_phase.value_or(""),
        record.jobs ? std::to_string(*record.jobs) : std::string{}, record.beside,
        record.cpu_seconds ? std::format("{:.1f}", *record.cpu_seconds) : std::string{},
        record.peak_memory ? std::to_string(*record.peak_memory) : std::string{});
}

namespace {

// A CSV field, quoted when it holds a comma, a quote or a line break.
std::string csv_field(std::string_view text) {
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string{text};
    }
    std::string quoted{"\""};
    for (const char c : text) {
        quoted += c;
        if (c == '"') {
            quoted += '"';
        }
    }
    quoted += '"';
    return quoted;
}

std::string joined(const std::vector<std::string>& words) {
    std::string found;
    for (const auto& word : words) {
        found += found.empty() ? word : " " + word;
    }
    return found;
}

std::string seconds_or_empty(std::optional<double> seconds) {
    return seconds ? std::format("{:.1f}", *seconds) : std::string{};
}

std::string count_or_empty(std::optional<std::uint64_t> count) {
    return count ? std::to_string(*count) : std::string{};
}

std::string share_or_empty(std::optional<double> share) {
    return share ? std::format("{:.3f}", *share) : std::string{};
}

} // namespace

std::string_view builds_csv_header() {
    return "start,origin,run,cpv,repo,slot,outcome,build_seconds,merge_seconds,waited,"
           "failed_phase,status,error,jobs,beside,cpu_seconds,peak_memory,read_bytes,write_bytes,"
           "measured,cpu_busy,load,stall_cpu,stall_memory,stall_io,build_stall_cpu,"
           "build_stall_memory,build_stall_io,build_dir_size,image_size,distfiles_size,"
           "installed_size,jobserver_jobs,jobserver_free,use,env_files,log";
}

std::string build_csv_row(const BuildRecord& record) {
    const auto seconds =
        std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(record.start)}};
    const auto system = record.system.value_or(SystemShare{});
    const auto stall = record.build_stall.value_or(StallShare{});
    const std::vector<std::string> fields{
        std::format("{:%Y-%m-%dT%H:%M:%SZ}", seconds),
        record.origin,
        record.run,
        record.cpv,
        record.repo,
        record.slot,
        std::string{outcome_name(record.outcome)},
        std::format("{:.1f}", record.build_seconds),
        seconds_or_empty(record.merge_seconds),
        seconds_or_empty(record.waited),
        record.failed_phase.value_or(""),
        record.status ? std::to_string(*record.status) : std::string{},
        record.error.value_or(""),
        record.jobs ? std::to_string(*record.jobs) : std::string{},
        std::to_string(record.beside),
        seconds_or_empty(record.cpu_seconds),
        count_or_empty(record.peak_memory),
        count_or_empty(record.read_bytes),
        count_or_empty(record.write_bytes),
        record.measured ? std::string{measured_name(*record.measured)} : std::string{},
        share_or_empty(system.cpu_busy),
        system.load ? std::format("{:.2f}", *system.load) : std::string{},
        share_or_empty(system.stall_cpu),
        share_or_empty(system.stall_memory),
        share_or_empty(system.stall_io),
        share_or_empty(stall.cpu),
        share_or_empty(stall.memory),
        share_or_empty(stall.io),
        count_or_empty(record.build_dir_size),
        count_or_empty(record.image_size),
        count_or_empty(record.distfiles_size),
        count_or_empty(record.installed_size),
        record.jobserver ? std::format("{:.1f}", record.jobserver->jobs) : std::string{},
        record.jobserver ? std::format("{:.2f}", record.jobserver->free) : std::string{},
        joined(record.use),
        joined(record.env_files),
        record.log.value_or("")};
    std::string row;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            row += ',';
        }
        row += csv_field(fields.at(i));
    }
    return row;
}

namespace {

// The CPU seconds a build's processes used between two probes: from its leaf once there is one,
// which held nothing before it was made.
std::optional<double> cpu_between(const Probe& before, const Probe& after) {
    if (after.cgroup && after.cgroup->cpu_usec) {
        const auto since = before.cgroup ? before.cgroup->cpu_usec.value_or(0) : 0;
        return static_cast<double>(*after.cgroup->cpu_usec > since ? *after.cgroup->cpu_usec - since
                                                                   : 0) /
               1e6;
    }
    if (!before.worker || !after.worker) {
        return std::nullopt;
    }
    return std::max(after.worker->children_cpu - before.worker->children_cpu, 0.0);
}

std::optional<std::uint64_t> grown(std::optional<std::uint64_t> before,
                                   std::optional<std::uint64_t> after) {
    if (!before || !after) {
        return std::nullopt;
    }
    return *after > *before ? *after - *before : 0;
}

// The share of seconds some tasks stalled, from PSI's microsecond totals.
std::optional<double> stalled(std::optional<std::uint64_t> before,
                              std::optional<std::uint64_t> after, double seconds) {
    if (!before || !after || *after < *before || seconds <= 0) {
        return std::nullopt;
    }
    return std::min(static_cast<double>(*after - *before) / 1e6 / seconds, 1.0);
}

std::optional<SystemShare> system_share(const std::optional<pressure::Sample>& before,
                                        const std::optional<pressure::Sample>& after, double loads,
                                        std::size_t load_samples, double seconds) {
    SystemShare share;
    for (const auto& each : {before, after}) {
        if (each && each->load) {
            loads += *each->load;
            ++load_samples;
        }
    }
    if (load_samples > 0) {
        share.load = loads / static_cast<double>(load_samples);
    }
    if (before && after) {
        share.cpu_busy = pressure::cpu_busy(*before, *after);
        share.stall_cpu = stalled(before->totals.cpu, after->totals.cpu, seconds);
        share.stall_memory = stalled(before->totals.memory, after->totals.memory, seconds);
        share.stall_io = stalled(before->totals.io, after->totals.io, seconds);
    }
    if (share == SystemShare{}) {
        return std::nullopt;
    }
    return share;
}

} // namespace

BuildRecorder::BuildRecorder(std::string origin, std::string run, std::vector<BuildStep> steps,
                             Toolchain toolchain, std::optional<std::uint32_t> jobs)
    : origin_{std::move(origin)}, run_{std::move(run)}, steps_{std::move(steps)},
      toolchain_{std::move(toolchain)}, jobs_{jobs}, times_(steps_.size()) {}

void BuildRecorder::sampled(std::size_t step, std::uint64_t memory, std::optional<double> load,
                            std::optional<JobserverShare> tokens) {
    auto& times = times_.at(step);
    if (!times.building) {
        return;
    }
    if (tokens) {
        times.tokens.jobs += tokens->jobs;
        times.tokens.free += tokens->free;
        ++times.token_samples;
    }
    times.peak_memory = std::max(times.peak_memory.value_or(0), memory);
    if (load) {
        times.loads += *load;
        ++times.load_samples;
    }
}

void BuildRecorder::end_phase(Times& times, double now, const Probe& probe) {
    if (times.phase && times.phase_start) {
        times.phases.push_back({.phase = std::move(*times.phase),
                                .seconds = now - *times.phase_start,
                                .cpu_seconds = cpu_between(times.phase_started, probe)});
    }
    times.phase.reset();
    times.phase_start.reset();
}

BuildRecord BuildRecorder::record(std::size_t step, BuildRecord::Outcome outcome) const {
    const auto& built = steps_.at(step);
    const auto& times = times_.at(step);
    BuildRecord found{.origin = origin_,
                      .run = run_,
                      .start = times.build_start.value_or(0),
                      .cpv = built.cpv,
                      .repo = built.repo,
                      .slot = built.slot,
                      .use = built.use,
                      .env_files = built.env_files,
                      .environment = times.environment,
                      .toolchain = times.toolchain,
                      .phases = times.phases,
                      .build_seconds = 0,
                      .merge_seconds = std::nullopt,
                      .waited = std::nullopt,
                      .outcome = outcome,
                      .failed_phase = std::nullopt,
                      .status = std::nullopt,
                      .log = std::nullopt,
                      .error = std::nullopt,
                      .jobs = jobs_,
                      .beside = times.beside};
    if (times.build_start && times.built) {
        found.build_seconds = *times.built - *times.build_start;
    }
    const auto& before = times.started;
    const auto& after = times.ended;
    found.cpu_seconds = cpu_between(before, after);
    if (before.worker && after.worker) {
        found.read_bytes = grown(before.worker->read_bytes, after.worker->read_bytes);
        found.write_bytes = grown(before.worker->write_bytes, after.worker->write_bytes);
    }
    found.peak_memory = times.peak_memory;
    if (found.cpu_seconds || found.peak_memory || found.read_bytes) {
        found.measured = Measured::proc;
    }
    // Its leaf, made for it alone, counted from nothing: exact where the controllers keep it.
    if (const auto& leaf = after.cgroup) {
        found.measured = Measured::cgroup;
        if (leaf->memory_peak) {
            found.peak_memory = leaf->memory_peak;
        }
        if (leaf->read_bytes && leaf->write_bytes) {
            found.read_bytes = leaf->read_bytes;
            found.write_bytes = leaf->write_bytes;
        }
        const auto share = [&](std::optional<std::uint64_t> total) {
            return stalled(std::uint64_t{0}, total, found.build_seconds);
        };
        const StallShare stall{.cpu = share(leaf->stalls.cpu),
                               .memory = share(leaf->stalls.memory),
                               .io = share(leaf->stalls.io)};
        if (stall != StallShare{}) {
            found.build_stall = stall;
        }
    }
    found.system = system_share(before.system, after.system, times.loads, times.load_samples,
                                found.build_seconds);
    const auto size = [&](std::string_view name) -> std::optional<std::uint64_t> {
        const auto found_size = times.sizes.find(std::string{name});
        return found_size != times.sizes.end() ? std::optional{found_size->second} : std::nullopt;
    };
    found.build_dir_size = size("build");
    found.image_size = size("image");
    found.distfiles_size = size("distfiles");
    if (times.token_samples > 0) {
        const auto samples = static_cast<double>(times.token_samples);
        found.jobserver = JobserverShare{.jobs = times.tokens.jobs / samples,
                                         .free = times.tokens.free / samples};
    }
    if (outcome == BuildRecord::Outcome::failed && times.failure) {
        if (times.failure->kind == WorkerEvent::Kind::failed) {
            found.failed_phase = times.failure->text;
            found.status = times.failure->status;
            found.log = times.failure->log;
        } else {
            found.error = times.failure->text;
        }
    }
    return found;
}

std::optional<BuildRecord> BuildRecorder::traced(std::size_t step, Traced what, double now,
                                                 const Probe& probe) {
    auto& times = times_.at(step);
    const auto& built = steps_.at(step);
    switch (what) {
    case Traced::build_started: {
        times.build_start = now;
        times.building = true;
        times.started = probe;
        times.peak_memory.reset();
        times.loads = 0;
        times.load_samples = 0;
        times.tokens = {};
        times.token_samples = 0;
        times.sizes.clear();
        times.toolchain.clear();
        for (const auto& [cp, slots] : toolchain_) {
            auto& versions = times.toolchain.try_emplace(cp).first->second;
            for (const auto& version : slots | std::views::values) {
                versions.push_back(version);
            }
        }
        const auto building = static_cast<std::uint32_t>(
            std::ranges::count_if(times_, [](const Times& each) { return each.building; }));
        for (auto& each : times_) {
            if (each.building) {
                each.beside = std::max(each.beside, building - 1);
            }
        }
        return std::nullopt;
    }
    case Traced::built:
        end_phase(times, now, probe);
        times.building = false;
        times.built = now;
        times.ended = probe;
        return std::nullopt;
    case Traced::build_failed:
        end_phase(times, now, probe);
        times.building = false;
        times.built = now;
        times.ended = probe;
        return record(step, BuildRecord::Outcome::failed);
    case Traced::merge_started:
        times.merge_start = now;
        return std::nullopt;
    case Traced::merged: {
        if (std::ranges::contains(toolchain_cps, std::string_view{built.cp})) {
            if (built.uninstall) {
                if (const auto found = toolchain_.find(built.cp); found != toolchain_.end()) {
                    std::erase_if(found->second,
                                  [&](const auto& each) { return each.second == built.version; });
                }
            } else {
                toolchain_.try_emplace(built.cp).first->second.insert_or_assign(built.slot,
                                                                                built.version);
            }
        }
        if (built.uninstall) {
            return std::nullopt;
        }
        auto found = record(step, BuildRecord::Outcome::merged);
        found.merge_seconds = now - times.merge_start.value_or(now);
        if (times.built && times.merge_start) {
            found.waited = *times.merge_start - *times.built;
        }
        return found;
    }
    case Traced::merge_failed:
        if (built.uninstall) {
            return std::nullopt;
        }
        return record(step, BuildRecord::Outcome::failed);
    case Traced::skipped:
        break;
    }
    return std::nullopt;
}

void BuildRecorder::reported(std::size_t step, const WorkerEvent& event, double now,
                             const Probe& probe) {
    using Kind = WorkerEvent::Kind;
    auto& times = times_.at(step);
    switch (event.kind) {
    case Kind::environment:
        times.environment = event.values;
        break;
    case Kind::sizes:
        times.sizes = event.sizes;
        break;
    case Kind::phase:
        // The merge's phase is the merge's time, not the build's.
        if (times.building) {
            end_phase(times, now, probe);
            times.phase = event.text;
            times.phase_start = now;
            times.phase_started = probe;
        }
        break;
    case Kind::failed:
    case Kind::error:
        times.failure = event;
        break;
    case Kind::built:
    case Kind::merged:
    case Kind::uninstalled:
        break;
    }
}

} // namespace egraph
