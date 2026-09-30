// Reads one JSON case a line, {"tokens": [...], "use": [...], "empty_true": bool}, and writes
// check_required_use's answer a line, {"satisfied": bool, "unsatisfied": "..."}, for
// builder/tests/test_required_use.py to hold against portage's.
#include "required_use.hpp"

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
            const auto result =
                egraph::check_required_use(views, enabled, input.at("empty_true").get<bool>());
            std::cout << nlohmann::json{{"satisfied", result.satisfied},
                                        {"unsatisfied", result.unsatisfied}}
                             .dump()
                      << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "required-use-shadow: " << e.what() << '\n';
        return 1;
    }
}
