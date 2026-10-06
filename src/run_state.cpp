#include "run_state.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <utility>
#include <variant>

namespace egraph {

namespace {

using Json = nlohmann::json;

constexpr std::array<std::pair<RunState::Status, std::string_view>, 3> status_names{
    {{RunState::Status::running, "running"},
     {RunState::Status::failed, "failed"},
     {RunState::Status::done, "done"}}};

std::optional<std::vector<std::string>> strings(const Json& json, std::string_view key) {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> values;
    for (const auto& each : *found) {
        if (!each.is_string()) {
            return std::nullopt;
        }
        values.push_back(each.get<std::string>());
    }
    return values;
}

} // namespace

std::filesystem::path run_state_path(const std::filesystem::path& eroot) {
    return eroot / "var/lib/egraph/exec.json";
}

std::string run_state_json(const RunState& state) {
    const auto status =
        std::ranges::find(status_names, state.status, &decltype(status_names)::value_type::first);
    auto failed = Json::array();
    for (const auto& failure : state.failed) {
        failed.push_back({{"cpv", failure.cpv}, {"log", failure.log}});
    }
    return Json{{"arguments", state.arguments},
                {"failed", std::move(failed)},
                {"merged", state.merged},
                {"status", status->second}}
        .dump();
}

std::expected<RunState, std::string> parse_run_state(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    auto arguments = strings(json, "arguments");
    auto merged = strings(json, "merged");
    if (!arguments || !merged) {
        return std::unexpected("no list of strings for arguments and merged");
    }
    const auto status = json.find("status");
    if (status == json.end() || !status->is_string()) {
        return std::unexpected("no status");
    }
    const auto name = status->get<std::string>();
    const auto found =
        std::ranges::find(status_names, name, &decltype(status_names)::value_type::second);
    if (found == status_names.end()) {
        return std::unexpected("an unknown status: " + name);
    }
    std::vector<RunState::Failure> failed;
    if (const auto failures = json.find("failed"); failures != json.end()) {
        if (!failures->is_array()) {
            return std::unexpected("failed is not a list");
        }
        for (const auto& failure : *failures) {
            const auto cpv = failure.find("cpv");
            const auto log = failure.find("log");
            if (!failure.is_object() || cpv == failure.end() || !cpv->is_string() ||
                log == failure.end() || !log->is_string()) {
                return std::unexpected("a failure without a cpv and a log");
            }
            failed.push_back({.cpv = cpv->get<std::string>(), .log = log->get<std::string>()});
        }
    }
    return RunState{.arguments = std::move(*arguments),
                    .merged = std::move(*merged),
                    .failed = std::move(failed),
                    .status = found->first};
}

std::vector<Step> resumed_steps(const Store& store, const Evaluated& evaluated, const Plan& plan,
                                std::vector<Step> steps, std::span<const std::string> merged) {
    const auto done = [&](const Step& step) {
        const auto* merge = std::get_if<MergeStep>(&step);
        if (merge == nullptr) {
            return false;
        }
        const auto cpv =
            evaluated.string(evaluated.candidates.at(plan.merges.at(merge->merge).candidate).cpv);
        return std::ranges::contains(merged, cpv) &&
               std::ranges::any_of(store.packages, [&](const Package& package) {
                   return store.string(package.cpv) == cpv;
               });
    };
    std::erase_if(steps, done);
    return steps;
}

} // namespace egraph
