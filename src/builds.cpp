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
        phases.push_back({{"phase", phase.phase}, {"seconds", phase.seconds}});
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
            record.phases.push_back({.phase = std::move(*phase), .seconds = *seconds});
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
    return std::format("{:.0f}\t{}\t{}\t{}\t{:.1f}\t{}\t{}\t{}\t{}", record.start, record.cpv,
                       record.repo, outcome_name(record.outcome), record.build_seconds,
                       record.merge_seconds ? std::format("{:.1f}", *record.merge_seconds)
                                            : std::string{},
                       record.failed_phase.value_or(""),
                       record.jobs ? std::to_string(*record.jobs) : std::string{}, record.beside);
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

} // namespace

std::string_view builds_csv_header() {
    return "start,origin,run,cpv,repo,slot,outcome,build_seconds,merge_seconds,waited,"
           "failed_phase,status,error,jobs,beside,use,env_files,log";
}

std::string build_csv_row(const BuildRecord& record) {
    const auto seconds =
        std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(record.start)}};
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

BuildRecorder::BuildRecorder(std::string origin, std::string run, std::vector<BuildStep> steps,
                             Toolchain toolchain, std::optional<std::uint32_t> jobs)
    : origin_{std::move(origin)}, run_{std::move(run)}, steps_{std::move(steps)},
      toolchain_{std::move(toolchain)}, jobs_{jobs}, times_(steps_.size()) {}

void BuildRecorder::end_phase(Times& times, double now) {
    if (times.phase && times.phase_start) {
        times.phases.push_back(
            {.phase = std::move(*times.phase), .seconds = now - *times.phase_start});
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

std::optional<BuildRecord> BuildRecorder::traced(std::size_t step, Traced what, double now) {
    auto& times = times_.at(step);
    const auto& built = steps_.at(step);
    switch (what) {
    case Traced::build_started: {
        times.build_start = now;
        times.building = true;
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
        end_phase(times, now);
        times.building = false;
        times.built = now;
        return std::nullopt;
    case Traced::build_failed:
        end_phase(times, now);
        times.building = false;
        times.built = now;
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

void BuildRecorder::reported(std::size_t step, const WorkerEvent& event, double now) {
    using Kind = WorkerEvent::Kind;
    auto& times = times_.at(step);
    switch (event.kind) {
    case Kind::environment:
        times.environment = event.values;
        break;
    case Kind::phase:
        // The merge's phase is the merge's time, not the build's.
        if (times.building) {
            end_phase(times, now);
            times.phase = event.text;
            times.phase_start = now;
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
