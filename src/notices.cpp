#include "notices.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <initializer_list>

namespace egraph {

using Json = nlohmann::json;

namespace {

// Whether each of an array's entries is an object with the string fields given.
bool all_have(const Json& list, std::initializer_list<std::string_view> fields) {
    return list.is_array() && std::ranges::all_of(list, [&](const Json& entry) {
               return entry.is_object() && std::ranges::all_of(fields, [&](std::string_view field) {
                          const auto found = entry.find(field);
                          return found != entry.end() && found->is_string();
                      });
           });
}

} // namespace

std::expected<Notices, std::string> parse_notices(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    const auto config = json.find("config");
    if (config == json.end() || !all_have(*config, {"file", "update"})) {
        return std::unexpected("config: not a list of files and their updates");
    }
    const auto news = json.find("news");
    if (news == json.end() || !all_have(*news, {"repo", "item", "title"})) {
        return std::unexpected("news: not a list of news items");
    }
    Notices notices;
    for (const auto& entry : *config) {
        notices.config.push_back({.file = entry.at("file").get<std::string>(),
                                  .update = entry.at("update").get<std::string>()});
    }
    for (const auto& entry : *news) {
        notices.news.push_back({.repo = entry.at("repo").get<std::string>(),
                                .item = entry.at("item").get<std::string>(),
                                .title = entry.at("title").get<std::string>()});
    }
    return notices;
}

std::vector<std::string> notice_lines(const Notices& notices) {
    std::vector<std::string> lines;
    lines.reserve(notices.config.size() + notices.news.size());
    for (const auto& [file, update] : notices.config) {
        lines.push_back(std::format("{}\tconfig\t{}", file, update));
    }
    for (const auto& [repo, item, title] : notices.news) {
        lines.push_back(std::format("{}\tnews\t{}\t{}", item, repo, title));
    }
    return lines;
}

} // namespace egraph
