#include "run_display.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

using egraph::RunDisplay;

namespace {

RunDisplay::Options live(std::size_t total, std::size_t width = 80) {
    return {.live = true, .human = true, .width = width, .total = total};
}

} // namespace

TEST_CASE("off a terminal every status is a line of its own") {
    std::ostringstream out;
    RunDisplay display{out, {.live = false, .human = true, .width = 80, .total = 2}};
    display.status(0, "app-misc/a-1", "fetch");
    display.finish(1, "app-misc/b-1", "merged", false);
    CHECK(out.str() == "(1 of 2) app-misc/a-1: fetch\n(2 of 2) app-misc/b-1: merged\n");
}

TEST_CASE("scripts get tab-separated lines and no escapes") {
    std::ostringstream out;
    RunDisplay display{out, {.live = false, .human = false, .width = 80, .total = 2}};
    display.status(0, "app-misc/a-1", "fetch");
    display.note("egraph: skipped");
    CHECK(out.str() == "1\tapp-misc/a-1\tfetch\negraph: skipped\n");
}

TEST_CASE("a live status replaces the last drawing instead of adding to it") {
    std::ostringstream out;
    RunDisplay display{out, live(2)};
    display.status(0, "app-misc/a-1", "fetch");
    const auto first = out.str();
    CHECK(first == "\x1b[J(1 of 2) app-misc/a-1: fetch\n0 of 2 done, 0 s elapsed\n");
    display.status(0, "app-misc/a-1", "compile");
    // Back over the two lines drawn, then the package and totals again.
    CHECK(out.str().substr(first.size()) ==
          "\r\x1b[2A\x1b[J(1 of 2) app-misc/a-1: compile\n0 of 2 done, 0 s elapsed\n");
}

TEST_CASE("a package that is done leaves the region and its line stays above") {
    std::ostringstream out;
    RunDisplay display{out, live(2)};
    display.status(0, "app-misc/a-1", "merge");
    display.status(1, "app-misc/b-1", "fetch");
    const auto before = out.str().size();
    display.finish(0, "app-misc/a-1", "merged", false);
    CHECK(out.str().substr(before) ==
          "\r\x1b[3A\x1b[J(1 of 2) app-misc/a-1: merged\n(2 of 2) app-misc/b-1: fetch\n"
          "1 of 2 done, 0 s elapsed\n");
    CHECK(display.done() == 1);
}

TEST_CASE("failures are counted in the totals") {
    std::ostringstream out;
    RunDisplay display{out, live(3)};
    display.finish(0, "app-misc/a-1", "failed with status 1", true);
    CHECK(display.failures() == 1);
    CHECK(out.str().ends_with("1 of 3 done, 1 failed, 0 s elapsed\n"));
}

TEST_CASE("the totals carry the elapsed time and the estimate when there is one") {
    std::ostringstream out;
    RunDisplay display{out, live(4)};
    display.tick(125, 3700);
    CHECK(out.str().ends_with("0 of 4 done, 2 min 05 s elapsed, about 1 h 01 min left\n"));
}

TEST_CASE("a live line is cut to leave the last column empty") {
    std::ostringstream out;
    RunDisplay display{out, live(1, 20)};
    display.status(0, "app-misc/long-name-1", "compile");
    CHECK(out.str().find("(1 of 1) app-misc/l\n") != std::string::npos);
}

TEST_CASE("a cut does not split a multibyte character") {
    std::ostringstream out;
    RunDisplay display{out, live(1, 15)};
    display.note("(1 of 1) \xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
    // Fourteen bytes would end inside the third character, so it goes whole.
    CHECK(out.str().find("(1 of 1) \xC3\xA9\xC3\xA9\n") != std::string::npos);
}

TEST_CASE("closing leaves only the totals under what scrolled") {
    std::ostringstream out;
    RunDisplay display{out, live(1)};
    display.status(0, "app-misc/a-1", "merge");
    const auto before = out.str().size();
    display.close();
    CHECK(out.str().substr(before) == "\r\x1b[2A\x1b[J0 of 1 done, 0 s elapsed\n");
}

TEST_CASE("durations read as the totals line gives them") {
    CHECK(egraph::run_duration(9) == "9 s");
    CHECK(egraph::run_duration(247) == "4 min 07 s");
    CHECK(egraph::run_duration(3900) == "1 h 05 min");
    CHECK(egraph::run_duration(-3) == "0 s");
}
