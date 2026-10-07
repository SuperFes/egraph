// Drives egraph's session bus layer for test_bus.py: one JSON request a line, one answer a line.
//   {"notify": {"replaces": 0, "summary": "...", "body": "...", "actions": [[key, label], ...],
//               "expire": -1}}                     -> {"id": N}
//   {"close": N}                                    -> {}
//   {"claim": name}                                 -> {"claimed": true or false}
//   {"wait": milliseconds}                          -> {"events": [{"kind", "id", "action"}]}
// waiting until there are events or the time passes. Any failure answers {"error": "..."}; a
// malformed request ends it.
#include "bus.hpp"
#include "os.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

Json events_json(const std::vector<egraph::bus::Event>& events) {
    auto list = Json::array();
    for (const auto& event : events) {
        list.push_back(
            {{"kind", event.kind == egraph::bus::Event::Kind::action ? "action" : "closed"},
             {"id", event.id},
             {"action", event.action}});
    }
    return {{"events", std::move(list)}};
}

Json answer(egraph::bus::Session& session, egraph::os::Watcher& watcher, const Json& request) {
    if (const auto notify = request.find("notify"); notify != request.end()) {
        egraph::bus::Notification notification{.replaces =
                                                   notify->value("replaces", std::uint32_t{0}),
                                               .summary = notify->at("summary").get<std::string>(),
                                               .body = notify->value("body", std::string{}),
                                               .actions = {},
                                               .expire = notify->value("expire", std::int32_t{-1})};
        for (const auto& action : notify->value("actions", Json::array())) {
            notification.actions.emplace_back(action.at(0).get<std::string>(),
                                              action.at(1).get<std::string>());
        }
        const auto id = session.notify(notification);
        return id ? Json{{"id", *id}} : Json{{"error", id.error()}};
    }
    if (const auto claim = request.find("claim"); claim != request.end()) {
        const auto claimed = session.claim(claim->get<std::string>());
        return claimed ? Json{{"claimed", *claimed}} : Json{{"error", claimed.error()}};
    }
    if (const auto close = request.find("close"); close != request.end()) {
        const auto closed = session.close(close->get<std::uint32_t>());
        return closed ? Json::object() : Json{{"error", closed.error()}};
    }
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds{request.at("wait").get<std::int64_t>()};
    while (true) {
        auto events = session.events();
        if (!events) {
            return {{"error", events.error()}};
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now());
        if (!events->empty() || left.count() <= 0) {
            return events_json(*events);
        }
        if (const auto woken = watcher.wait(left, session.pollable()); !woken) {
            return {{"error", woken.error().message()}};
        }
    }
}

} // namespace

int main() {
    try {
        auto session = egraph::bus::Session::open();
        if (!session) {
            std::cout << Json{{"error", session.error()}}.dump() << '\n';
            return 1;
        }
        auto watcher = egraph::os::Watcher::open();
        if (!watcher) {
            std::cout << Json{{"error", watcher.error().message()}}.dump() << '\n';
            return 1;
        }
        std::string line;
        while (std::getline(std::cin, line)) {
            std::cout << answer(*session, *watcher, Json::parse(line)).dump() << '\n' << std::flush;
        }
    } catch (const std::exception& e) {
        std::cerr << "bus-probe: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
