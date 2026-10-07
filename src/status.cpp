#include "status.hpp"

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <locale>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace egraph {

namespace {

using Json = nlohmann::json;

std::optional<std::uint64_t> number_at(const Json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::uint64_t>();
}

std::optional<std::size_t> count_at(const Json& object, std::string_view key) {
    return number_at(object, key).transform([](std::uint64_t count) {
        return static_cast<std::size_t>(count);
    });
}

std::optional<Seconds> time_at(const Json& object, std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    return Seconds{std::chrono::seconds{found->get<std::int64_t>()}};
}

std::optional<StoreBuild> build_of(const Json& stores, std::string_view key) {
    const auto found = stores.find(key);
    if (found == stores.end() || !found->is_object()) {
        return std::nullopt;
    }
    const auto path = found->find("path");
    const auto built = number_at(*found, "built");
    if (path == found->end() || !path->is_string() || !built) {
        return std::nullopt;
    }
    return StoreBuild{.path = path->get<std::string>(), .built = *built};
}

std::optional<StatusStores> stores_of(const Json& stores) {
    if (!stores.is_object()) {
        return std::nullopt;
    }
    auto installed = build_of(stores, "installed");
    auto evaluated = build_of(stores, "evaluated");
    auto repository = build_of(stores, "repository");
    if (!installed || !evaluated || !repository) {
        return std::nullopt;
    }
    return StatusStores{.installed = std::move(*installed),
                        .evaluated = std::move(*evaluated),
                        .repository = std::move(*repository)};
}

Json build_json(const StoreBuild& build) {
    return {{"path", build.path}, {"built", build.built}};
}

std::optional<PlanCounts> counts_of(const Json& counts) {
    if (!counts.is_object()) {
        return std::nullopt;
    }
    PlanCounts read;
    for (auto [key, field] : {std::pair{"upgrades", &read.upgrades},
                              {"downgrades", &read.downgrades},
                              {"rebuilds", &read.rebuilds},
                              {"new", &read.added},
                              {"held", &read.held},
                              {"uninstalls", &read.uninstalls},
                              {"masked", &read.masked}}) {
        const auto count = count_at(counts, key);
        if (!count) {
            return std::nullopt;
        }
        *field = *count;
    }
    const auto refused = counts.find("refused");
    if (refused == counts.end() || !refused->is_boolean()) {
        return std::nullopt;
    }
    read.refused = refused->get<bool>();
    return read;
}

Json counts_json(const PlanCounts& counts) {
    return {{"upgrades", counts.upgrades}, {"downgrades", counts.downgrades},
            {"rebuilds", counts.rebuilds}, {"new", counts.added},
            {"held", counts.held},         {"uninstalls", counts.uninstalls},
            {"masked", counts.masked},     {"refused", counts.refused}};
}

constexpr std::array input_kinds{
    std::pair{InputKind::file, std::string_view{"file"}},
    std::pair{InputKind::directory, std::string_view{"directory"}},
    std::pair{InputKind::symlink, std::string_view{"symlink"}},
    std::pair{InputKind::missing, std::string_view{"missing"}},
};

Json inputs_json(std::span<const Input> inputs) {
    auto list = Json::array();
    for (const auto& input : inputs) {
        const auto kind =
            std::ranges::find(input_kinds, input.kind, &decltype(input_kinds)::value_type::first);
        list.push_back({{"path", input.path},
                        {"kind", kind->second},
                        {"mtime", input.mtime_ns},
                        {"size", input.size}});
    }
    return list;
}

std::optional<std::vector<Input>> inputs_of(const Json& inputs) {
    if (!inputs.is_array()) {
        return std::nullopt;
    }
    std::vector<Input> read;
    for (const auto& entry : inputs) {
        const auto path = entry.is_object() ? entry.find("path") : entry.end();
        const auto kind = entry.is_object() ? entry.find("kind") : entry.end();
        const auto mtime = number_at(entry, "mtime");
        const auto size = number_at(entry, "size");
        if (path == entry.end() || !path->is_string() || kind == entry.end() ||
            !kind->is_string() || !mtime || !size) {
            return std::nullopt;
        }
        const auto known = std::ranges::find(input_kinds, kind->get<std::string>(),
                                             &decltype(input_kinds)::value_type::second);
        if (known == input_kinds.end()) {
            return std::nullopt;
        }
        read.push_back({.path = path->get<std::string>(),
                        .kind = known->first,
                        .mtime_ns = *mtime,
                        .size = *size});
    }
    return read;
}

std::optional<std::vector<RepositorySync>> repositories_of(const Json& repositories) {
    if (!repositories.is_array()) {
        return std::nullopt;
    }
    std::vector<RepositorySync> read;
    for (const auto& repository : repositories) {
        const auto name = repository.is_object() ? repository.find("name") : repository.end();
        if (name == repository.end() || !name->is_string()) {
            return std::nullopt;
        }
        read.push_back({.name = name->get<std::string>(), .synced = time_at(repository, "synced")});
    }
    return read;
}

std::optional<std::vector<std::string>> lines_of(const Json& lines) {
    if (!lines.is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> read;
    for (const auto& line : lines) {
        if (!line.is_string()) {
            return std::nullopt;
        }
        read.push_back(line.get<std::string>());
    }
    return read;
}

} // namespace

PlanCounts plan_counts(const Plan& plan) {
    PlanCounts counts{.held = plan.held.size(),
                      .uninstalls = plan.uninstalls.size(),
                      .masked = plan.masked.size(),
                      .refused = plan.refused()};
    for (const auto& merge : plan.merges) {
        if (!merge.replaces) {
            ++counts.added;
        } else if (merge.kind == UpdateKind::upgrade) {
            ++counts.upgrades;
        } else if (merge.kind == UpdateKind::downgrade) {
            ++counts.downgrades;
        } else {
            ++counts.rebuilds;
        }
    }
    return counts;
}

std::optional<Seconds> parse_sync_timestamp(std::string_view text) {
    std::istringstream in{std::string{text.substr(0, text.find('\n'))}};
    in.imbue(std::locale::classic());
    Seconds time;
    in >> std::chrono::parse("%a, %d %b %Y %H:%M:%S %z", time);
    if (!in) {
        return std::nullopt;
    }
    return time;
}

std::optional<Seconds> repository_synced(const std::filesystem::path& location) {
    std::ifstream in{location / "metadata/timestamp.chk"};
    std::string line;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    return parse_sync_timestamp(line);
}

namespace {

bool under(std::string_view path, const std::filesystem::path& directory) {
    auto prefix = directory.lexically_normal().string();
    while (prefix.size() > 1 && prefix.ends_with('/')) {
        prefix.pop_back();
    }
    return path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() &&
                              path.at(prefix.size()) == '/');
}

std::vector<Input> layer_inputs(std::span<const std::span<const Input>> layers,
                                const std::filesystem::path& config_dir, bool config) {
    std::vector<Input> found;
    for (const auto layer : layers) {
        for (const auto& input : layer) {
            if (under(input.path, config_dir) == config) {
                found.push_back(input);
            }
        }
    }
    std::ranges::sort(found);
    const auto [first, last] = std::ranges::unique(found);
    found.erase(first, last);
    return found;
}

// The lines a plan change compares: without those that only explain another.
std::vector<std::string> entries(std::span<const std::string> lines) {
    std::vector<std::string> kept;
    for (const auto& line : lines) {
        const auto tab = line.find('\t');
        const auto kind =
            tab == std::string::npos
                ? std::string_view{}
                : std::string_view{line}.substr(tab + 1, line.find('\t', tab + 1) - tab - 1);
        if (kind != "holder" && kind != "nodeps") {
            kept.push_back(line);
        }
    }
    std::ranges::sort(kept);
    return kept;
}

std::vector<std::string> difference(std::span<const std::string> from,
                                    std::span<const std::string> less) {
    std::vector<std::string> found;
    std::ranges::set_difference(from, less, std::back_inserter(found));
    return found;
}

std::string spaced(std::string line) {
    std::ranges::replace(line, '\t', ' ');
    return line;
}

} // namespace

std::vector<Input> config_inputs(std::span<const std::span<const Input>> layers,
                                 const std::filesystem::path& config_dir) {
    return layer_inputs(layers, config_dir, true);
}

std::string other_inputs_digest(std::span<const std::span<const Input>> layers,
                                const std::filesystem::path& config_dir) {
    // FNV-1a: stable from one egraph to the next, as std::hash is not.
    std::uint64_t digest = 0xcbf29ce484222325U;
    const auto add = [&digest](std::uint64_t byte) { digest = (digest ^ byte) * 0x100000001b3U; };
    const auto add_number = [&add](std::uint64_t number) {
        for (int shift = 0; shift < 64; shift += 8) {
            add((number >> shift) & 0xffU);
        }
    };
    for (const auto& input : layer_inputs(layers, config_dir, false)) {
        for (const char c : input.path) {
            add(static_cast<unsigned char>(c));
        }
        add(0);
        add(std::to_underlying(input.kind));
        add_number(input.mtime_ns);
        add_number(input.size);
    }
    return std::format("{:016x}", digest);
}

std::optional<PlanChange> plan_change(const Status& before, const Status& after) {
    if (before.others.empty() || before.others != after.others || before.config == after.config) {
        return std::nullopt;
    }
    const auto was = entries(before.lines);
    const auto now = entries(after.lines);
    if (was == now && before.counts == after.counts) {
        return std::nullopt;
    }
    std::vector<std::string> files;
    std::vector<Input> moved;
    std::ranges::set_symmetric_difference(before.config, after.config, std::back_inserter(moved));
    for (const auto& input : moved) {
        files.push_back(input.path);
    }
    std::ranges::sort(files);
    const auto [first, last] = std::ranges::unique(files);
    files.erase(first, last);
    return PlanChange{.files = std::move(files),
                      .before = before.counts,
                      .gained = difference(now, was),
                      .lost = difference(was, now)};
}

std::optional<Notice> plan_notice(const Status& status) {
    if (!status.change) {
        return std::nullopt;
    }
    const auto& change = *status.change;
    const auto& before = change.before;
    const auto& after = status.counts;
    std::vector<std::string> parts;
    for (const auto& [was, now, one, many] :
         {std::tuple{before.upgrades, after.upgrades, "upgrade", "upgrades"},
          {before.downgrades, after.downgrades, "downgrade", "downgrades"},
          {before.rebuilds, after.rebuilds, "rebuild", "rebuilds"},
          {before.added, after.added, "new", "new"},
          {before.held, after.held, "held", "held"},
          {before.uninstalls, after.uninstalls, "uninstall", "uninstalls"},
          {before.masked, after.masked, "masked", "masked"}}) {
        if (was != now) {
            const auto by = now > was ? now - was : was - now;
            parts.push_back(
                std::format("{}{} {}", now > was ? '+' : '-', by, by == 1 ? one : many));
        }
    }
    if (before.refused != after.refused) {
        parts.emplace_back(after.refused ? "now refused" : "no longer refused");
    }
    std::string title = "Configuration edit: ";
    if (parts.empty()) {
        title += "the plan changed";
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        title += (i == 0 ? "" : ", ") + parts.at(i);
    }
    std::vector<std::string> detail;
    for (const auto& file : change.files) {
        detail.push_back("edited " + file);
    }
    for (const auto& line : change.gained) {
        detail.push_back("+ " + spaced(line));
    }
    for (const auto& line : change.lost) {
        detail.push_back("- " + spaced(line));
    }
    return Notice{.kind = NoticeKind::plan,
                  .key = "plan",
                  .title = std::move(title),
                  .detail = std::move(detail),
                  .fingerprint = std::format("{}", status.written.time_since_epoch().count()),
                  .since = status.written};
}

std::string status_json(const Status& status, std::optional<bool> current) {
    auto repositories = Json::array();
    for (const auto& repository : status.repositories) {
        Json entry{{"name", repository.name}};
        if (repository.synced) {
            entry.emplace("synced", repository.synced->time_since_epoch().count());
        }
        repositories.push_back(std::move(entry));
    }
    const auto& counts = status.counts;
    auto stores = Json::object();
    stores.emplace("installed", build_json(status.stores.installed));
    stores.emplace("evaluated", build_json(status.stores.evaluated));
    stores.emplace("repository", build_json(status.stores.repository));
    Json document{{"format", status_format},
                  {"command", std::string{status_command}},
                  {"written", status.written.time_since_epoch().count()},
                  {"stores", std::move(stores)},
                  {"counts", counts_json(counts)},
                  {"repositories", std::move(repositories)},
                  {"lines", status.lines}};
    if (!status.others.empty()) {
        document.emplace("config", inputs_json(status.config));
        document.emplace("others", status.others);
    }
    if (status.change) {
        const auto& change = *status.change;
        document.emplace("change", Json{{"files", change.files},
                                        {"before", counts_json(change.before)},
                                        {"gained", change.gained},
                                        {"lost", change.lost}});
    }
    if (current) {
        document.emplace("current", *current);
    }
    return document.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
}

std::expected<Status, std::string> parse_status(std::string_view text) {
    const auto document = Json::parse(text, nullptr, false);
    if (!document.is_object()) {
        return std::unexpected("not a status file");
    }
    const auto format = number_at(document, "format");
    if (format && *format != status_format) {
        return std::unexpected(std::format("format {}, from another egraph version", *format));
    }
    const auto at = [&document](std::string_view key) -> const Json& {
        static const Json missing;
        const auto found = document.find(key);
        return found == document.end() ? missing : *found;
    };
    const auto written = time_at(document, "written");
    auto stores = stores_of(at("stores"));
    auto counts = counts_of(at("counts"));
    auto repositories = repositories_of(at("repositories"));
    auto lines = lines_of(at("lines"));
    if (!format || !written || !stores || !counts || !repositories || !lines) {
        return std::unexpected("not a status file");
    }
    Status status{.written = *written,
                  .stores = *stores,
                  .counts = *counts,
                  .repositories = std::move(*repositories),
                  .lines = std::move(*lines)};
    // Kept since the plan change; a status without them is read as one from before.
    if (const auto& others = at("others"); others.is_string()) {
        auto config = inputs_of(at("config"));
        if (!config) {
            return std::unexpected("not a status file");
        }
        status.config = std::move(*config);
        status.others = others.get<std::string>();
    }
    if (const auto& change = at("change"); !change.is_null()) {
        const auto field = [&change](std::string_view key) -> const Json& {
            static const Json missing;
            const auto found = change.is_object() ? change.find(key) : change.end();
            return found == change.end() ? missing : *found;
        };
        auto files = lines_of(field("files"));
        auto before = counts_of(field("before"));
        auto gained = lines_of(field("gained"));
        auto lost = lines_of(field("lost"));
        if (!files || !before || !gained || !lost) {
            return std::unexpected("not a status file");
        }
        status.change = PlanChange{.files = std::move(*files),
                                   .before = *before,
                                   .gained = std::move(*gained),
                                   .lost = std::move(*lost)};
    }
    return status;
}

std::filesystem::path status_path(const std::filesystem::path& installed) {
    return installed.parent_path() / "status.json";
}

bool stores_unchanged(const StatusStores& stores) {
    const auto built = [](const std::expected<std::uint64_t, StoreError>& time) {
        return time ? std::optional{*time} : std::nullopt;
    };
    return built(store_build_time(stores.installed.path)) == stores.installed.built &&
           built(evaluated_build_time(stores.evaluated.path)) == stores.evaluated.built &&
           built(repository_build_time(stores.repository.path)) == stores.repository.built;
}

std::string age_text(Seconds then, Seconds now) {
    using namespace std::chrono;
    const auto age = now - then;
    const auto ago = [](auto count, std::string_view unit) {
        return std::format("{} {}{} ago", count, unit, count == 1 ? "" : "s");
    };
    if (age >= days{1}) {
        return ago(floor<days>(age).count(), "day");
    }
    if (age >= hours{1}) {
        return ago(floor<hours>(age).count(), "hour");
    }
    if (age >= minutes{1}) {
        return ago(floor<minutes>(age).count(), "minute");
    }
    return "just now";
}

std::vector<std::string> status_summary(const Status& status, bool current, Seconds now) {
    const auto& counts = status.counts;
    std::string line;
    for (const auto& [count, one, many] : {std::tuple{counts.upgrades, "upgrade", "upgrades"},
                                           {counts.downgrades, "downgrade", "downgrades"},
                                           {counts.rebuilds, "rebuild", "rebuilds"},
                                           {counts.added, "new", "new"},
                                           {counts.held, "held", "held"},
                                           {counts.uninstalls, "uninstall", "uninstalls"},
                                           {counts.masked, "masked", "masked"}}) {
        if (count != 0) {
            line +=
                std::format("{}{} {}", line.empty() ? "" : ", ", count, count == 1 ? one : many);
        }
    }
    if (line.empty()) {
        line = "no updates";
    }
    if (counts.refused) {
        line += "; emerge would refuse the plan";
    }
    std::vector<std::string> lines{std::move(line)};
    for (const auto& repository : status.repositories) {
        if (repository.synced) {
            lines.push_back(
                std::format("{} synced {}", repository.name, age_text(*repository.synced, now)));
        }
    }
    lines.push_back(std::format("planned {}{}", age_text(status.written, now),
                                current ? "" : ", before the stores last changed"));
    return lines;
}

std::vector<std::string> status_lines(const Status& status, bool current) {
    const auto& counts = status.counts;
    const auto yes = [](bool value) { return value ? "yes" : "no"; };
    std::vector<std::string> lines{
        std::format("upgrades\t{}", counts.upgrades),
        std::format("downgrades\t{}", counts.downgrades),
        std::format("rebuilds\t{}", counts.rebuilds),
        std::format("new\t{}", counts.added),
        std::format("held\t{}", counts.held),
        std::format("uninstalls\t{}", counts.uninstalls),
        std::format("masked\t{}", counts.masked),
        std::format("refused\t{}", yes(counts.refused)),
        std::format("current\t{}", yes(current)),
        std::format("written\t{}", status.written.time_since_epoch().count())};
    for (const auto& repository : status.repositories) {
        lines.push_back(std::format(
            "synced\t{}\t{}", repository.name,
            repository.synced ? std::format("{}", repository.synced->time_since_epoch().count())
                              : ""));
    }
    return lines;
}

bool status_due(PlanWhen when, const std::optional<StatusStores>& recorded,
                const StatusStores& now) {
    switch (when) {
    case PlanWhen::never:
        return false;
    case PlanWhen::sync:
        return !recorded || recorded->repository != now.repository;
    case PlanWhen::refresh:
        return !recorded || *recorded != now;
    }
    return true;
}

std::string notice_file_json(const NoticeFile& file) {
    auto repositories = Json::array();
    for (const auto& repository : file.repositories) {
        Json entry{{"name", repository.name}};
        if (repository.synced) {
            entry.emplace("synced", repository.synced->time_since_epoch().count());
        }
        repositories.push_back(std::move(entry));
    }
    auto stores = Json::object();
    stores.emplace("installed", build_json(file.stores.installed));
    stores.emplace("evaluated", build_json(file.stores.evaluated));
    stores.emplace("repository", build_json(file.stores.repository));
    auto notices = Json::array();
    for (const auto& notice : file.notices) {
        notices.push_back({{"kind", notice_kind_name(notice.kind)},
                           {"key", notice.key},
                           {"title", notice.title},
                           {"detail", notice.detail},
                           {"packages", notice.packages},
                           {"file", notice.file},
                           {"fingerprint", notice.fingerprint},
                           {"since", notice.since.time_since_epoch().count()}});
    }
    const Json document{{"format", notice_file_format},
                        {"written", file.written.time_since_epoch().count()},
                        {"stores", std::move(stores)},
                        {"repositories", std::move(repositories)},
                        {"notices", std::move(notices)}};
    return document.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
}

std::expected<NoticeFile, std::string> parse_notice_file(std::string_view text) {
    const auto document = Json::parse(text, nullptr, false);
    if (!document.is_object()) {
        return std::unexpected("not a notices file");
    }
    const auto format = number_at(document, "format");
    if (format && *format != notice_file_format) {
        return std::unexpected(std::format("format {}, from another egraph version", *format));
    }
    const auto at = [&document](std::string_view key) -> const Json& {
        static const Json missing;
        const auto found = document.find(key);
        return found == document.end() ? missing : *found;
    };
    const auto written = time_at(document, "written");
    auto stores = stores_of(at("stores"));
    auto repositories = repositories_of(at("repositories"));
    const auto& listed = at("notices");
    if (!format || !written || !stores || !repositories || !listed.is_array()) {
        return std::unexpected("not a notices file");
    }
    NoticeFile file{.written = *written,
                    .stores = *stores,
                    .repositories = std::move(*repositories),
                    .notices = {}};
    for (const auto& entry : listed) {
        if (!entry.is_object()) {
            return std::unexpected("not a notices file");
        }
        const auto text_at = [&entry](std::string_view key) -> std::optional<std::string> {
            const auto found = entry.find(key);
            if (found == entry.end() || !found->is_string()) {
                return std::nullopt;
            }
            return found->get<std::string>();
        };
        const auto kind_name = text_at("kind");
        const auto kind = kind_name ? notice_kind(*kind_name) : std::nullopt;
        auto key = text_at("key");
        auto title = text_at("title");
        auto fingerprint = text_at("fingerprint");
        auto detail = lines_of(entry.value("detail", Json{}));
        // Absent from the files egraph wrote before notices named their packages.
        auto packages = lines_of(entry.value("packages", Json::array()));
        auto file_name = entry.value("file", Json(std::string{}));
        const auto since = time_at(entry, "since");
        if (!kind || !key || !title || !fingerprint || !detail || !packages ||
            !file_name.is_string() || !since) {
            return std::unexpected("not a notices file");
        }
        file.notices.push_back({.kind = *kind,
                                .key = std::move(*key),
                                .title = std::move(*title),
                                .detail = std::move(*detail),
                                .packages = std::move(*packages),
                                .file = file_name.get<std::string>(),
                                .fingerprint = std::move(*fingerprint),
                                .since = *since});
    }
    return file;
}

std::filesystem::path notices_path(const std::filesystem::path& installed) {
    return installed.parent_path() / "notices.json";
}

std::vector<Notice> current_notices(const NoticeFile& file, Seconds now, int stale_days) {
    std::vector<Notice> notices;
    std::ranges::copy_if(file.notices, std::back_inserter(notices),
                         [](const Notice& notice) { return notice.kind != NoticeKind::stale; });
    if (stale_days <= 0) {
        return notices;
    }
    const std::chrono::days allowed{stale_days};
    Notices stale;
    for (const auto& repository : file.repositories) {
        if (repository.synced && *repository.synced < now - allowed) {
            stale.stale.push_back({.name = repository.name, .synced = *repository.synced});
        }
    }
    auto found = notice_list(stale, now);
    // When each went stale, unless the file saw it before; the list holds them alone, in order.
    for (std::size_t i = 0; i < found.size(); ++i) {
        found.at(i).since = stale.stale.at(i).synced + allowed;
    }
    carry_since(found, file.notices);
    notices.insert(notices.end(), std::make_move_iterator(found.begin()),
                   std::make_move_iterator(found.end()));
    return notices;
}

std::optional<std::string> notices_summary(std::span<const Notice> notices) {
    if (notices.empty()) {
        return std::nullopt;
    }
    const auto count = [&](NoticeKind kind) {
        return std::ranges::count(notices, kind, &Notice::kind);
    };
    std::string parts;
    const auto add = [&parts](std::string part) {
        parts += std::format("{}{}", parts.empty() ? "" : ", ", part);
    };
    for (const auto& [kind, one, many] :
         {std::tuple{NoticeKind::glsa, "GLSA", "GLSAs"},
          {NoticeKind::missing, "package missing libraries", "packages missing libraries"},
          {NoticeKind::masked, "masked package", "masked packages"},
          {NoticeKind::stale, "stale repository", "stale repositories"},
          {NoticeKind::news, "news item", "news items"}}) {
        if (const auto n = count(kind); n != 0) {
            add(std::format("{} {}", n, n == 1 ? one : many));
        }
        if (kind == NoticeKind::missing && count(NoticeKind::preserved) != 0) {
            add("preserved libraries");
        }
        if (kind == NoticeKind::stale && count(NoticeKind::config) != 0) {
            add("configuration updates");
        }
        if (kind == NoticeKind::stale && count(NoticeKind::plan) != 0) {
            add("the plan changed by an edit");
        }
    }
    return std::format("{} {}: {}", notices.size(), notices.size() == 1 ? "notice" : "notices",
                       parts);
}

} // namespace egraph
