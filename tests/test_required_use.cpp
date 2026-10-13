#include "required_use.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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

egraph::RequiredUse check(std::string_view required, std::string_view use,
                          bool empty_true = false) {
    const auto flags = split(use);
    return egraph::check_required_use(split(required), {flags.begin(), flags.end()}, empty_true);
}

} // namespace

// Expectations are portage's check_required_use and its tree's tounicode().
TEST_CASE("REQUIRED_USE operators weigh their terms") {
    CHECK_FALSE(check("^^ ( a b )", "").satisfied);
    CHECK(check("^^ ( a b )", "a").satisfied);
    CHECK_FALSE(check("^^ ( a b )", "a b").satisfied);
    CHECK(check("?? ( a b )", "").satisfied);
    CHECK_FALSE(check("?? ( a b )", "a b").satisfied);
    CHECK(check("|| ( a b )", "b").satisfied);
    CHECK_FALSE(check("a !b", "a b").satisfied);
    CHECK(check("a? ( b )", "").satisfied);
    CHECK(check("!a? ( b )", "a").satisfied);
    CHECK(check("", "").satisfied);
}

TEST_CASE("an empty group holds only where the EAPI says so") {
    CHECK_FALSE(check("|| ( )", "").satisfied);
    CHECK(check("|| ( )", "", true).satisfied);
    CHECK(check("|| ( )", "").unsatisfied.empty());
}

TEST_CASE("the unsatisfied constraints drop what holds, but whole under an operator") {
    CHECK(check("^^ ( a b )", "").unsatisfied == "^^ ( a b )");
    CHECK(check("^^ ( a b )", "a").unsatisfied.empty());
    CHECK(check("x? ( || ( a b ) ) c? ( a ) !x? ( b )", "x").unsatisfied == "x? ( || ( a b ) )");
    CHECK(check("?? ( a ( b c ) ) || ( a )", "a b c").unsatisfied == "?? ( a ( b c ) )");
    CHECK(check("a b", "b").unsatisfied == "a");
    CHECK(check("!a? ( b ) a? ( ^^ ( b c ) )", "a b c").unsatisfied == "a? ( ^^ ( b c ) )");
}

TEST_CASE("groups flatten as portage's tree re-parents them") {
    CHECK(check("a? ( b ( c d ) )", "a b").unsatisfied == "a? ( c d )");
    CHECK(check("|| ( a ( b c ) )", "b").unsatisfied == "|| ( a ( b c ) )");
    CHECK(check("^^ ( ( a ) b )", "a b").unsatisfied == "^^ ( a b )");
    CHECK(check("x? ( ( a b ) )", "x").unsatisfied == "x? ( a b )");
    // An operator left with one term gives way to it.
    CHECK(check("|| ( a? ( b ) c )", "").unsatisfied == "c");
    CHECK(check("|| ( ( a ) )", "").unsatisfied == "a");
}

TEST_CASE("operators read as portage spells them out") {
    CHECK(egraph::human_readable_required_use("^^ ( a b ) || ( c ?? ( d e ) )") ==
          "exactly-one-of ( a b ) any-of ( c at-most-one-of ( d e ) )");
}

namespace {

egraph::Mend mend(std::string_view required, std::string_view use, std::string_view free,
                  std::string_view flag) {
    const auto flags = split(use);
    const auto changeable = split(free);
    return egraph::mend_required_use(split(required), {flags.begin(), flags.end()}, changeable,
                                     flag, false);
}

using Mends = std::vector<std::vector<std::string>>;

} // namespace

TEST_CASE("a toggle that keeps REQUIRED_USE satisfied needs no mending") {
    const auto kept = mend("^^ ( a b )", "a", "a b", "x");
    CHECK(kept.broken.empty());
    CHECK(kept.mends.empty());
    // Unsatisfied already, a toggle breaks nothing, mending it or not.
    CHECK(mend("^^ ( a b )", "a b", "a b x", "x").broken.empty());
    CHECK(mend("^^ ( a b )", "", "a b", "a").broken.empty());
}

TEST_CASE("a toggle that breaks REQUIRED_USE is mended by the fewest other toggles") {
    // On with another on: the other goes off.
    auto broken = mend("^^ ( a b c )", "a", "a b c", "b");
    CHECK(broken.broken == "^^ ( a b c )");
    CHECK(broken.mends == Mends{{"-a"}});
    CHECK(mend("?? ( a b )", "a", "a b", "b").mends == Mends{{"-a"}});
    // Off with nothing else on: either of the others, which is for the user to pick.
    CHECK(mend("^^ ( a b c )", "a", "a b c", "a").mends == Mends{{"b"}, {"c"}});
    CHECK(mend("|| ( a b )", "a", "a b", "a").mends == Mends{{"b"}});
    // A conditional's flag on brings what it needs.
    CHECK(mend("x? ( y z )", "", "x y z", "x").mends == Mends{{"y", "z"}});
    // Flags it does not name never mend it.
    CHECK(mend("|| ( a b )", "a", "a b unrelated", "a").mends == Mends{{"b"}});
    // What cannot change does not mend it.
    const auto fixed = mend("x? ( y )", "", "x", "x");
    CHECK(fixed.broken == "x? ( y )");
    CHECK(fixed.mends.empty());
}
