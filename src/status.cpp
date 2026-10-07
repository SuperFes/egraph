#include "status.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <locale>
#include <sstream>
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

std::optional<StatusStores> stores_of(const Json& stores) {
    if (!stores.is_object()) {
        return std::nullopt;
    }
    const auto installed = number_at(stores, "installed");
    const auto evaluated = number_at(stores, "evaluated");
    const auto repository = number_at(stores, "repository");
    if (!installed || !evaluated || !repository) {
        return std::nullopt;
    }
    return StatusStores{
        .installed = *installed, .evaluated = *evaluated, .repository = *repository};
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

std::string status_json(const Status& status) {
    auto repositories = Json::array();
    for (const auto& repository : status.repositories) {
        Json entry{{"name", repository.name}};
        if (repository.synced) {
            entry.emplace("synced", repository.synced->time_since_epoch().count());
        }
        repositories.push_back(std::move(entry));
    }
    const auto& counts = status.counts;
    const Json document{{"format", status_format},
                        {"command", std::string{status_command}},
                        {"written", status.written.time_since_epoch().count()},
                        {"stores",
                         {{"installed", status.stores.installed},
                          {"evaluated", status.stores.evaluated},
                          {"repository", status.stores.repository}}},
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

} // namespace egraph
