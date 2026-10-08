#include "restart.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

using egraph::Replacement;
using egraph::os::FileIdentity;
using std::chrono::milliseconds;

namespace {

Replacement::Clock::time_point at(milliseconds since) {
    return Replacement::Clock::time_point{} + since;
}

constexpr FileIdentity running{.device = 1, .inode = 10, .mtime_ns = 100, .size = 1000};
constexpr FileIdentity replaced{.device = 1, .inode = 11, .mtime_ns = 200, .size = 1000};

} // namespace

TEST_CASE("the executable unchanged is never due") {
    Replacement replacement{running};
    CHECK_FALSE(replacement.seen(running, at(milliseconds{0})));
    CHECK_FALSE(replacement.seen(running, at(milliseconds{5000})));
    CHECK_FALSE(replacement.wait(at(milliseconds{5000})));
}

TEST_CASE("a replacement is due once it has gone a second unchanged") {
    Replacement replacement{running};
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{0})));
    CHECK(replacement.wait(at(milliseconds{0})) == Replacement::settle);
    CHECK(replacement.wait(at(milliseconds{400})) == milliseconds{600});
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{999})));
    CHECK(replacement.seen(replaced, at(milliseconds{1000})));
}

TEST_CASE("a replacement still being written, or gone, starts the second over") {
    Replacement replacement{running};
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{0})));
    auto larger = replaced;
    larger.size = 2000;
    CHECK_FALSE(replacement.seen(larger, at(milliseconds{800})));
    CHECK_FALSE(replacement.seen(larger, at(milliseconds{1500})));
    CHECK(replacement.seen(larger, at(milliseconds{1800})));
    // Missing between an unlink and a rename.
    CHECK_FALSE(replacement.seen(std::nullopt, at(milliseconds{1900})));
    CHECK_FALSE(replacement.wait(at(milliseconds{1900})));
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{2000})));
    CHECK(replacement.seen(replaced, at(milliseconds{3000})));
}

TEST_CASE("after a restart fails, the replacement counts as running until replaced again") {
    Replacement replacement{running};
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{0})));
    CHECK(replacement.seen(replaced, at(milliseconds{1000})));
    replacement.keep();
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{5000})));
    CHECK_FALSE(replacement.wait(at(milliseconds{5000})));
    auto again = replaced;
    again.inode = 12;
    CHECK_FALSE(replacement.seen(again, at(milliseconds{6000})));
    CHECK(replacement.seen(again, at(milliseconds{7000})));
}

TEST_CASE("without knowing what runs, nothing is due") {
    Replacement replacement{std::nullopt};
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{0})));
    CHECK_FALSE(replacement.seen(replaced, at(milliseconds{5000})));
}
