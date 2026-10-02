#include "os.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

egraph::os::Talk talk(const std::string& script) {
    auto started = egraph::os::start_talking({"sh", "-c", script});
    REQUIRE(started.has_value());
    return std::move(*started);
}

} // namespace

TEST_CASE("a talking child answers each line sent to it") {
    auto child = talk(R"(while read -r line; do echo "got $line"; done; exit 3)");
    CHECK(child.send("a"));
    CHECK(child.receive() == "got a");
    CHECK(child.send("b c"));
    CHECK(child.receive() == "got b c");
    // Its input closed, it ends.
    CHECK(child.finish() == 3);
}

TEST_CASE("a talking child's last line need not end in a newline") {
    auto child = talk(R"(printf 'one\ntwo')");
    CHECK(child.receive() == "one");
    CHECK(child.receive() == "two");
    CHECK(child.receive() == std::nullopt);
    CHECK(child.finish() == 0);
}

TEST_CASE("sending to a child that has ended fails without a signal") {
    auto child = talk("exit 0");
    CHECK(child.receive() == std::nullopt);
    CHECK(child.finish() == 0);
    CHECK_FALSE(child.send("anyone there"));
}

TEST_CASE("a talking child that cannot start is an error") {
    CHECK_FALSE(egraph::os::start_talking({"/nonexistent/egraph-test"}).has_value());
    CHECK_FALSE(egraph::os::start_talking({}).has_value());
}
