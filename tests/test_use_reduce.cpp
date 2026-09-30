#include "use_reduce.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> tokens;
    while (!text.empty()) {
        const auto start = text.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            break;
        }
        text.remove_prefix(start);
        const auto end = std::min(text.find(' '), text.size());
        tokens.push_back(text.substr(0, end));
        text.remove_prefix(end);
    }
    return tokens;
}

// "type parent text" per node, as the builder's tests print node tuples: 0 atom, 1 any-of,
// 2 all-of, 3 weak blocker, 4 strong blocker; -1 for no parent.
std::vector<std::string> reduced(std::string_view dependencies, std::string_view use,
                                 bool empty_true = false) {
    const auto flags = split(use);
    std::vector<std::string> lines;
    for (const auto& node : egraph::reduce_dependencies(split(dependencies),
                                                        {flags.begin(), flags.end()}, empty_true)) {
        lines.push_back(std::format(
            "{} {} {}", static_cast<int>(node.type),
            node.parent == egraph::no_parent ? -1 : static_cast<int>(node.parent), node.text));
    }
    return lines;
}

using Lines = std::vector<std::string>;

} // namespace

// Expectations are portage's use_reduce laid out by installed.nodes.
TEST_CASE("conditionals keep what their flag's state selects") {
    CHECK(reduced("a/b c/d", "") == Lines{"0 -1 a/b", "0 -1 c/d"});
    CHECK(reduced("x? ( a/b ) !x? ( c/d )", "x") == Lines{"0 -1 a/b"});
    CHECK(reduced("a/b x? ( c/d ) || ( e/f x? ( g/h ) )", "") == Lines{"0 -1 a/b", "0 -1 e/f"});
    CHECK(reduced("x? ( || ( a/b c/d ) ) e/f", "x") ==
          Lines{"1 -1 ", "0 0 a/b", "0 0 c/d", "0 -1 e/f"});
}

TEST_CASE("redundant groups go as use_reduce drops them") {
    CHECK(reduced("|| ( a/b c/d )", "") == Lines{"1 -1 ", "0 0 a/b", "0 0 c/d"});
    CHECK(reduced("|| ( a/b )", "") == Lines{"0 -1 a/b"});
    CHECK(reduced("|| ( x? ( a/b ) c/d )", "") == Lines{"0 -1 c/d"});
    CHECK(reduced("|| ( x? ( a/b ) c/d )", "x") == Lines{"1 -1 ", "0 0 a/b", "0 0 c/d"});
    CHECK(reduced("|| ( ( a/b c/d ) e/f )", "") ==
          Lines{"1 -1 ", "2 0 ", "0 1 a/b", "0 1 c/d", "0 0 e/f"});
    CHECK(reduced("( a/b c/d )", "") == Lines{"0 -1 a/b", "0 -1 c/d"});
    CHECK(reduced("x? ( ( a/b c/d ) )", "x") == Lines{"0 -1 a/b", "0 -1 c/d"});
    CHECK(reduced("|| ( x? ( a/b e/f ) c/d )", "x") ==
          Lines{"1 -1 ", "2 0 ", "0 1 a/b", "0 1 e/f", "0 0 c/d"});
    CHECK(reduced("|| ( || ( a/b c/d ) e/f )", "") ==
          Lines{"1 -1 ", "0 0 a/b", "0 0 c/d", "0 0 e/f"});
    CHECK(reduced("|| ( a/b ( c/d ) )", "") == Lines{"1 -1 ", "0 0 a/b", "0 0 c/d"});
    CHECK(reduced("|| ( ( a/b ) c/d )", "") == Lines{"1 -1 ", "0 0 a/b", "0 0 c/d"});
    CHECK(reduced("|| ( x? ( || ( a/b c/d ) ) e/f )", "x") ==
          Lines{"1 -1 ", "0 0 a/b", "0 0 c/d", "0 0 e/f"});
    CHECK(reduced("|| ( x? ( ( a/b ) ) )", "x") == Lines{"0 -1 a/b"});
}

TEST_CASE("a || left empty holds only where the EAPI says so") {
    CHECK(reduced("|| ( x? ( a/b ) )", "") == Lines{"0 -1 __const__/empty-any-of"});
    CHECK(reduced("|| ( x? ( a/b ) )", "", true).empty());
    CHECK(reduced("|| ( x? ( a/b ) ) c/d", "", true) == Lines{"0 -1 c/d"});
}

TEST_CASE("atoms' USE conditionals are evaluated, and blockers typed") {
    CHECK(reduced("a/b[x?,-y,z=,!w=,!v?] !c/d", "x w") ==
          Lines{"0 -1 a/b[x,-y,-z,-w,-v]", "3 -1 !c/d"});
    CHECK(reduced("!!a/b[x=]", "") == Lines{"4 -1 !!a/b[-x]"});
    CHECK(reduced("a/b:1/2=[x?] >=a/c-1:*[!x=] a/d:=[x(+)?,y(-)=]", "x") ==
          Lines{"0 -1 a/b:1/2=[x]", "0 -1 >=a/c-1:*[-x]", "0 -1 a/d:=[x(+),-y(-)]"});
    // Nothing left of them, the brackets go too.
    CHECK(egraph::evaluate_use_conditionals("a/b:0[x?]", {}) == "a/b:0");
    CHECK(egraph::evaluate_use_conditionals("a/b[x,y]", {}) == "a/b[x,y]");
}
