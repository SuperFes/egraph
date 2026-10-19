#include "minimize.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <string>
#include <vector>

using egraph::minimize_children;

TEST_CASE("a package another atom's selection covers is dropped") {
    // python:3.14 selects 14, a bare python 16; both match 14.
    const std::array order{14, 16};
    std::vector<std::vector<int>> lists{{14}, {14, 16}};
    minimize_children<int>(order, lists);
    CHECK(lists == std::vector<std::vector<int>>{{14}, {14}});
}

TEST_CASE("packages only one atom matches stay") {
    const std::array order{1, 2};
    std::vector<std::vector<int>> lists{{1}, {2}};
    minimize_children<int>(order, lists);
    CHECK(lists == std::vector<std::vector<int>>{{1}, {2}});
}

TEST_CASE("the lowest goes first when either would do") {
    // Two version ranges both matching 1 and 2: 1 goes, leaving 2 for both.
    const std::array order{1, 2};
    std::vector<std::vector<int>> lists{{1, 2}, {1, 2}};
    minimize_children<int>(order, lists);
    CHECK(lists == std::vector<std::vector<int>>{{2}, {2}});
}

TEST_CASE("lists are reached through a projection") {
    const std::array order{14, 16};
    std::map<std::string, std::vector<int>> lists{{"python", {14, 16}}, {"python:3.14", {14}}};
    minimize_children<int>(order, lists, &decltype(lists)::value_type::second);
    CHECK(lists.at("python") == std::vector<int>{14});
    CHECK(lists.at("python:3.14") == std::vector<int>{14});
}
