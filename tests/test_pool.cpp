#include "pool.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Answers each line with "got" and the line, and ends on "end".
const std::vector<std::string> echoing{
    "sh", "-c", R"(while read -r line; do [ "$line" = end ] && exit 4; echo "got $line"; done)"};

egraph::Heard heard(egraph::WorkerPool& pool, bool for_token = false) {
    auto next = pool.next(for_token);
    REQUIRE(next.has_value());
    return std::move(*next);
}

} // namespace

TEST_CASE("a pool's workers are started as added and heard from together") {
    std::ostringstream notes;
    egraph::WorkerPool pool{echoing, std::nullopt, false, {}, notes};
    CHECK(pool.add() == 0);
    CHECK(pool.add() == 1);
    CHECK(pool.send(1, "b"));
    auto first = heard(pool);
    CHECK(first.worker == 1);
    CHECK(first.line == "got b");
    CHECK(pool.send(0, "a"));
    CHECK(pool.send(1, "end"));
    std::set<std::pair<std::size_t, std::optional<std::string>>> found;
    for (int i = 0; i < 2; ++i) {
        auto next = heard(pool);
        REQUIRE(next.worker);
        found.emplace(*next.worker, next.line);
    }
    // A worker's end is heard once; it takes no more.
    CHECK(found == std::set<std::pair<std::size_t, std::optional<std::string>>>{{0, "got a"},
                                                                                {1, std::nullopt}});
    CHECK_FALSE(pool.send(1, "more"));
    CHECK(pool.send(0, "c"));
    CHECK(heard(pool).line == "got c");
    // The first exit status that is not 0.
    CHECK(pool.finish() == 4);
}

TEST_CASE("a pool's workers end with their input") {
    std::ostringstream notes;
    egraph::WorkerPool pool{echoing, std::nullopt, false, {}, notes};
    CHECK(pool.add() == 0);
    CHECK(pool.finish() == 0);
}

TEST_CASE("without a jobserver every build has a token") {
    std::ostringstream notes;
    egraph::WorkerPool pool{echoing, std::nullopt, false, {}, notes};
    for (std::size_t step = 0; step < 3; ++step) {
        CHECK(pool.take_token(step) == true);
    }
    CHECK(pool.give_token(1).has_value());
}

TEST_CASE("builds take a jobserver's tokens, the first the implicit one, and give them back") {
    const egraph::test::TempDir dir;
    const auto fifo = dir.path() / "jobserver";
    REQUIRE(egraph::os::run({"mkfifo", fifo.string()}) == 0);
    auto jobserver = egraph::os::Jobserver::open(fifo);
    REQUIRE(jobserver.has_value());
    REQUIRE(jobserver->give(std::byte{'+'}).has_value());
    std::ostringstream notes;
    egraph::WorkerPool pool{echoing, std::move(*jobserver), true, {}, notes};
    CHECK(pool.take_token(0) == true);
    CHECK(pool.take_token(1) == true);
    CHECK(pool.take_token(2) == false);
    // The implicit token back, then the jobserver's, which lets a worker wake us.
    CHECK(pool.give_token(0).has_value());
    CHECK(pool.take_token(2) == true);
    CHECK(pool.take_token(3) == false);
    CHECK(pool.give_token(1).has_value());
    const auto woke = heard(pool, true);
    CHECK_FALSE(woke.worker);
    CHECK(pool.take_token(3) == true);
}

TEST_CASE("a pool with nothing to wait for says so") {
    std::ostringstream notes;
    egraph::WorkerPool pool{echoing, std::nullopt, false, {}, notes};
    CHECK_FALSE(pool.next(false).has_value());
}

TEST_CASE("a build beside others needs room in the build directory, as emerge weighs it") {
    const egraph::test::TempDir dir;
    std::ostringstream notes;
    egraph::WorkerPool unchecked{
        echoing, std::nullopt, false, {.tmpdir = dir.path(), .free_gb = 0}, notes};
    CHECK(unchecked.room_for(100));
    egraph::WorkerPool small{
        echoing, std::nullopt, false, {.tmpdir = dir.path(), .free_gb = 1}, notes};
    CHECK(small.room_for(0));
    // More than any disk, and more bytes than a count holds.
    egraph::WorkerPool huge{
        echoing, std::nullopt, false, {.tmpdir = dir.path(), .free_gb = 1ULL << 50}, notes};
    CHECK_FALSE(huge.room_for(1));
    CHECK_FALSE(huge.room_for(1));
    // Said once.
    CHECK(notes.str().find("builds wait to run alone") != std::string::npos);
    CHECK(notes.str().find("builds wait") == notes.str().rfind("builds wait"));
}
