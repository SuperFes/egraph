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

bool strings(const Json& list) {
    return list.is_array() && std::ranges::all_of(list, &Json::is_string);
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
    const auto preserved = json.find("preserved");
    if (preserved == json.end() ||
        !(preserved->is_null() || (all_have(*preserved, {"path", "package"}) &&
                                   std::ranges::all_of(*preserved, [](const Json& entry) {
                                       return strings(entry.value("consumers", Json{}));
                                   })))) {
        return std::unexpected("preserved: not a list of libraries and what uses them");
    }
    const auto rebuild = json.find("rebuild");
    if (rebuild == json.end() || !(rebuild->is_null() || strings(*rebuild))) {
        return std::unexpected("rebuild: not a list of atoms");
    }
    Notices notices;
    if (!preserved->is_null()) {
        auto& found = notices.preserved.emplace();
        for (const auto& entry : *preserved) {
            found.push_back({.path = entry.at("path").get<std::string>(),
                             .package = entry.at("package").get<std::string>(),
                             .consumers = entry.at("consumers").get<std::vector<std::string>>()});
        }
    }
    if (!rebuild->is_null()) {
        notices.rebuild = rebuild->get<std::vector<std::string>>();
    }
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
    for (const auto& [path, package, consumers] :
         notices.preserved.value_or(std::vector<Notices::Preserved>{})) {
        std::string joined;
        for (const auto& consumer : consumers) {
            joined += std::format("{}{}", joined.empty() ? "" : " ", consumer);
        }
        lines.push_back(std::format("{}\tpreserved\t{}\t{}", path, package, joined));
    }
    for (const auto& atom : notices.rebuild.value_or(std::vector<std::string>{})) {
        lines.push_back(std::format("{}\trebuild", atom));
    }
    return lines;
}

} // namespace egraph
