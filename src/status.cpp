#include "status.hpp"

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

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
                  {"counts",
                   {{"upgrades", counts.upgrades},
                    {"downgrades", counts.downgrades},
                    {"rebuilds", counts.rebuilds},
                    {"new", counts.added},
                    {"held", counts.held},
                    {"uninstalls", counts.uninstalls},
                    {"masked", counts.masked},
                    {"refused", counts.refused}}},
                  {"repositories", std::move(repositories)},
                  {"lines", status.lines}};
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
    return Status{.written = *written,
                  .stores = *stores,
                  .counts = *counts,
                  .repositories = std::move(*repositories),
                  .lines = std::move(*lines)};
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
        const auto since = time_at(entry, "since");
        if (!kind || !key || !title || !fingerprint || !detail || !packages || !since) {
            return std::unexpected("not a notices file");
        }
        file.notices.push_back({.kind = *kind,
                                .key = std::move(*key),
                                .title = std::move(*title),
                                .detail = std::move(*detail),
                                .packages = std::move(*packages),
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
    }
    return std::format("{} {}: {}", notices.size(), notices.size() == 1 ? "notice" : "notices",
                       parts);
}

} // namespace egraph
