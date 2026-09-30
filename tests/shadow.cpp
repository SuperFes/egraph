// Reads one JSON case a line and writes egraph's answer a line, for the builder's tests to hold
// against portage's:
//   {"check": "required_use", "tokens": [...], "use": [...], "empty_true": bool}
//     -> {"satisfied": bool, "unsatisfied": "..."}
//   {"check": "use_reduce", "tokens": [...], "use": [...], "empty_true": bool}
//     -> {"nodes": [[type, parent, text], ...]}, parent -1 for none
#include "required_use.hpp"
#include "use_reduce.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

int main() {
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto input = nlohmann::json::parse(line);
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
