// Reads one JSON case a line and writes egraph's answer a line, for the builder's tests to hold
// against portage's:
//   {"check": "required_use", "tokens": [...], "use": [...], "empty_true": bool}
//     -> {"satisfied": bool, "unsatisfied": "..."}
//   {"check": "use_reduce", "tokens": [...], "use": [...], "empty_true": bool}
//     -> {"nodes": [[type, parent, text], ...]}, parent -1 for none
//   {"check": "myopts", "options": [...]} -> the options as emerge's parser leaves them
//   {"check": "visibility", "index": path} -> the index's visibility ledger stacked, in the
//     shape of its visibility section
#include "repository.hpp"
#include "required_use.hpp"
#include "resume.hpp"
#include "use_reduce.hpp"
#include "visibility_stack.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using egraph::SourcedToken;
using egraph::StackedKey;

std::vector<std::string> tokens_of(const std::vector<SourcedToken>& tokens) {
    return tokens | std::views::transform(&SourcedToken::token) | std::ranges::to<std::vector>();
}

// The builder's _net: the last * or -*, then the last word on each name after it, sorted.
std::vector<std::string> net(const std::vector<std::string>& tokens) {
    std::optional<std::string> reset;
    std::map<std::string, std::string> last;
    for (const auto& token : tokens) {
        if (token == "*" || token == "-*") {
            reset = token;
            last.clear();
        } else {
            last[token.starts_with('-') ? token.substr(1) : token] = token;
        }
    }
    std::vector<std::string> found;
    if (reset) {
        found.push_back(*reset);
    }
    for (const auto& [name, token] : last) {
        found.push_back(token);
    }
    std::ranges::sort(found.begin() + (reset ? 1 : 0), found.end());
    return found;
}

nlohmann::json keys_json(const std::vector<StackedKey>& keys, bool netted = false) {
    auto found = nlohmann::json::array();
    for (const auto& key : keys) {
        const auto tokens = tokens_of(key.tokens);
        found.push_back({{"atom", key.atom}, {"tokens", netted ? net(tokens) : tokens}});
    }
    return found;
}

nlohmann::json visibility_json(const std::string& path) {
    const auto index = egraph::load_repository(path);
    if (!index) {
        throw std::runtime_error{"cannot load " + path};
    }
    const auto stacked = egraph::stack_visibility(*index);
    auto accept_keywords = tokens_of(stacked.accept_keywords);
    std::ranges::sort(accept_keywords);
    const auto masks = stacked.masks | std::views::transform(&egraph::StackedMask::atom) |
                       std::ranges::to<std::vector>();
    const auto unmasks = stacked.unmasks | std::views::transform(&egraph::StackedMask::atom) |
                         std::ranges::to<std::vector>();
    auto profile_keywords = nlohmann::json::array();
    auto profile_accept_keywords = nlohmann::json::array();
    for (const auto& [layers, out] :
         {std::pair{&stacked.profile_keywords, &profile_keywords},
          std::pair{&stacked.profile_accept_keywords, &profile_accept_keywords}}) {
        for (const auto& layer : *layers) {
            if (!layer.empty()) {
                out->push_back(keys_json(layer));
            }
        }
    }
    return {{"accept_keywords", accept_keywords},
            {"environment_keywords", tokens_of(stacked.environment_keywords)},
            {"profile_keywords", profile_keywords},
            {"profile_accept_keywords", profile_accept_keywords},
            {"accept_keywords_entries", keys_json(stacked.accept_keywords_entries)},
            {"masks", masks},
            {"unmasks", unmasks},
            {"accept_license", net(tokens_of(stacked.accept_license))},
            {"licenses", keys_json(stacked.licenses, true)},
            {"accept_properties", tokens_of(stacked.accept_properties)},
            {"properties", keys_json(stacked.properties)},
            {"accept_restrict", tokens_of(stacked.accept_restrict)},
            {"restrict", keys_json(stacked.restrict)}};
}

} // namespace

int main() {
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto input = nlohmann::json::parse(line);
            if (input.at("check") == "myopts") {
                const auto options = input.at("options").get<std::vector<std::string>>();
                std::cout << egraph::emerge_myopts(options) << '\n';
                continue;
            }
            if (input.at("check") == "visibility") {
                std::cout << visibility_json(input.at("index").get<std::string>()).dump() << '\n';
                continue;
            }
            const auto tokens = input.at("tokens").get<std::vector<std::string>>();
            const auto use = input.at("use").get<std::vector<std::string>>();
            const std::vector<std::string_view> views(tokens.begin(), tokens.end());
            const std::set<std::string_view> enabled(use.begin(), use.end());
            const auto empty_true = input.at("empty_true").get<bool>();
            nlohmann::json output;
            if (input.at("check") == "required_use") {
                const auto result = egraph::check_required_use(views, enabled, empty_true);
                output = {{"satisfied", result.satisfied}, {"unsatisfied", result.unsatisfied}};
            } else {
                auto nodes = nlohmann::json::array();
                for (const auto& node : egraph::reduce_dependencies(views, enabled, empty_true)) {
                    nodes.push_back({static_cast<int>(node.type),
                                     node.parent == egraph::no_parent
                                         ? -1
                                         : static_cast<long long>(node.parent),
                                     node.text});
                }
                output = {{"nodes", nodes}};
            }
            std::cout << output.dump() << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "shadow: " << e.what() << '\n';
        return 1;
    }
}
