#include "cli.hpp"
#include "helpers.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
#include <string>

using egraph::test::TempDir;
using egraph::test::write_bytes;

namespace {

struct Ran {
    egraph::Exit exit = egraph::Exit::ok;
    std::string out;
    std::string err;
};

// The shell over a stale store (the sample records an input the system does not have) that
// never refreshes, so that each load of it warns.
Ran shell(const std::string& input, bool prompt = false) {
    const TempDir dir;
    write_bytes(dir.path() / "installed.egraph",
                egraph::test::assemble(egraph::test::sample_sections()));
    egraph::Invocation invocation;
    invocation.store = dir.path() / "installed.egraph";
    invocation.builder = "/nonexistent/egraph-build";
    invocation.no_refresh = true;
    std::istringstream in{input};
    std::ostringstream out;
    std::ostringstream err;
    const auto exit = egraph::shell(invocation, in, out, err, prompt);
    return {.exit = exit, .out = out.str(), .err = err.str()};
}

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t found = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++found;
    }
    return found;
}

} // namespace

TEST_CASE("the shell answers each line from one session") {
    const auto ran = shell("match dev-libs/b\n\n# a comment\nsoname --providers libb.so.1\n");
    CHECK(ran.exit == egraph::Exit::ok);
    CHECK(ran.out == "dev-libs/b\tdev-libs/b-1\ndev-libs/b-1\tx86_64\n");
    // The store was loaded once.
    CHECK(count(ran.err, "answering from a stale store") == 1);
}

TEST_CASE("a line takes global options for itself") {
    const auto ran = shell("--layout lines match dev-libs/b\nmatch app-misc/a\n");
    CHECK(ran.out == "dev-libs/b\tdev-libs/b-1\napp-misc/a\tapp-misc/a-1\n");
}

TEST_CASE("a failing line is reported and the shell carries on") {
    const auto ran = shell("nonsense\nmatch '>=dev-libs/b-2'\nmatch dev-libs/b\n");
    CHECK(ran.out == "dev-libs/b\tdev-libs/b-1\n");
    CHECK(ran.err.starts_with("egraph: shell: nonsense: no such command"));
    CHECK(ran.exit == egraph::Exit::ok);
    CHECK(shell("match dev-libs/b\nsoname --bogus\n").exit == egraph::Exit::usage);
}

TEST_CASE("the stores stay the session's") {
    const auto ran = shell("--root /mnt match dev-libs/b\n--store /x stats\nshell\n");
    CHECK(ran.out.empty());
    CHECK(count(ran.err, "egraph: shell: ") == 3);
    CHECK(ran.err.find("--root") != std::string::npos);
}

TEST_CASE("quit ends the shell, and help prints the usage") {
    const auto ran = shell("help\nquit\nmatch dev-libs/b\n");
    CHECK(ran.out.find("SUBCOMMANDS:") != std::string::npos);
    CHECK(ran.out.find("dev-libs/b-1") == std::string::npos);
    CHECK(shell("exit\nmatch dev-libs/b\n").out.empty());
}

TEST_CASE("the shell prompts when asked") {
    const auto ran = shell("match dev-libs/b\n", true);
    CHECK(ran.out == "egraph> dev-libs/b\tdev-libs/b-1\negraph> \n");
}
