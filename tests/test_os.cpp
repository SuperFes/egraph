#include "os.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

egraph::os::Talk talk(const std::string& script) {
    auto started = egraph::os::start_talking({"sh", "-c", script});
    REQUIRE(started.has_value());
    return std::move(*started);
}

std::optional<std::byte> taken(egraph::os::Jobserver& jobserver) {
    auto token = jobserver.take();
    REQUIRE(token.has_value());
    return *token;
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

TEST_CASE("waiting on talking children finds those with a line or an end") {
    std::vector<egraph::os::Talk> talks;
    talks.push_back(talk(R"(read -r line; sleep 0.3; echo slow)"));
    talks.push_back(talk(R"(read -r line; printf 'qu'; sleep 0.1; echo ick)"));
    CHECK(talks.at(0).send("go"));
    CHECK(talks.at(1).send("go"));
    auto found = egraph::os::wait_for(talks);
    REQUIRE(found.has_value());
    // Not before its whole line has come.
    CHECK(found->talks == std::vector<std::size_t>{1});
    CHECK_FALSE(found->token);
    CHECK(talks.at(1).receive() == "quick");
    found = egraph::os::wait_for(talks);
    REQUIRE(found.has_value());
    CHECK(found->talks == std::vector<std::size_t>{1});
    CHECK(talks.at(1).receive() == std::nullopt);
    // An ended talk stays ready.
    found = egraph::os::wait_for(talks);
    REQUIRE(found.has_value());
    CHECK(found->talks == std::vector<std::size_t>{1});
    CHECK(talks.at(0).ready() == false);
    CHECK(talks.at(1).finish() == 0);
    talks.pop_back();
    found = egraph::os::wait_for(talks);
    REQUIRE(found.has_value());
    CHECK(found->talks == std::vector<std::size_t>{0});
    CHECK(talks.at(0).receive() == "slow");
    CHECK(talks.at(0).finish() == 0);
}

TEST_CASE("a wait given a timeout ends with nothing found once it passes") {
    std::vector<egraph::os::Talk> talks;
    talks.push_back(talk(R"(read -r line; printf 'par'; sleep 2; echo tial)"));
    CHECK(talks.at(0).send("go"));
    const auto started = std::chrono::steady_clock::now();
    const auto found = egraph::os::wait_for(talks, std::nullopt, std::chrono::milliseconds{200});
    REQUIRE(found.has_value());
    // A part of a line that comes meanwhile does not end it early.
    CHECK(found->talks.empty());
    CHECK(found->timed_out);
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds{200});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{1500});
}

TEST_CASE("a talking child's pid, and this process's, are the ones the system knows") {
    auto child = talk(R"(read -r line; echo $$)");
    CHECK(child.send("go"));
    CHECK(child.receive() == std::to_string(child.pid()));
    CHECK(child.finish() == 0);
    CHECK(std::to_string(egraph::os::process_id()) ==
          std::filesystem::read_symlink("/proc/self").string());
}

TEST_CASE("nothing to wait for finds nothing") {
    const auto found = egraph::os::wait_for({});
    REQUIRE(found.has_value());
    CHECK(found->talks.empty());
    CHECK_FALSE(found->token);
}

TEST_CASE("a jobserver's tokens are taken and given back, and waited for") {
    const egraph::test::TempDir dir;
    const auto fifo = dir.path() / "jobserver";
    REQUIRE(egraph::os::run({"mkfifo", fifo.string()}) == 0);
    auto jobserver = egraph::os::Jobserver::open(fifo);
    REQUIRE(jobserver.has_value());
    CHECK(taken(*jobserver) == std::nullopt);
    // A token another process gives back.
    std::vector<egraph::os::Talk> talks;
    talks.push_back(
        talk(std::format("sleep 0.2; printf + > {}; read -r line; exit 0", fifo.string())));
    const auto found = egraph::os::wait_for(talks, std::cref(*jobserver));
    REQUIRE(found.has_value());
    CHECK(found->token);
    CHECK(found->talks.empty());
    CHECK(taken(*jobserver) == std::byte{'+'});
    CHECK(taken(*jobserver) == std::nullopt);
    CHECK(jobserver->give(std::byte{'+'}).has_value());
    CHECK(taken(*jobserver) == std::byte{'+'});
    CHECK(talks.at(0).finish() == 0);
}

TEST_CASE("free space is that of the nearest directory that exists") {
    const egraph::test::TempDir dir;
    const auto free = egraph::os::free_bytes(dir.path() / "not" / "yet");
    REQUIRE(free.has_value());
    CHECK(*free > 0);
    CHECK(free == egraph::os::free_bytes(dir.path()));
}

TEST_CASE("a watcher wakes for an entry made, written or removed in a directory it watches") {
    const egraph::test::TempDir dir;
    std::filesystem::create_directory(dir.path() / "watched");
    std::filesystem::create_directory(dir.path() / "other");
    auto watcher = egraph::os::Watcher::open();
    REQUIRE(watcher.has_value());
    REQUIRE(watcher->add(dir.path() / "watched"));
    CHECK_FALSE(watcher->add(dir.path() / "missing"));
    const auto quiet = [&] {
        const auto woken = watcher->wait(std::chrono::milliseconds{0});
        REQUIRE(woken.has_value());
        return !woken->changed && !woken->stop;
    };
    const auto changed = [&] {
        const auto woken = watcher->wait(std::chrono::milliseconds{5000});
        REQUIRE(woken.has_value());
        return woken->changed;
    };
    CHECK(quiet());
    egraph::test::write_text(dir.path() / "other/file", "x");
    CHECK(quiet());
    egraph::test::write_text(dir.path() / "watched/file", "x");
    CHECK(changed());
    // The events are drained: one wake for all that came.
    CHECK(quiet());
    std::filesystem::remove(dir.path() / "watched/file");
    CHECK(changed());
}

TEST_CASE("a directory watched aside wakes a wait without counting as changed") {
    const egraph::test::TempDir dir;
    std::filesystem::create_directory(dir.path() / "watched");
    std::filesystem::create_directory(dir.path() / "aside");
    std::filesystem::create_directory(dir.path() / "both");
    auto watcher = egraph::os::Watcher::open();
    REQUIRE(watcher.has_value());
    REQUIRE(watcher->add(dir.path() / "watched"));
    REQUIRE(watcher->add_aside(dir.path() / "aside"));
    REQUIRE(watcher->add_aside(dir.path() / "both"));
    REQUIRE(watcher->add(dir.path() / "both"));
    const auto woken = [&] {
        const auto found = watcher->wait(std::chrono::milliseconds{5000});
        REQUIRE(found.has_value());
        return std::pair{found->changed, found->aside};
    };
    egraph::test::write_text(dir.path() / "aside/file", "x");
    CHECK(woken() == std::pair{false, true});
    egraph::test::write_text(dir.path() / "watched/file", "x");
    CHECK(woken() == std::pair{true, false});
    // Watched both ways, it counts.
    egraph::test::write_text(dir.path() / "both/file", "x");
    CHECK(woken() == std::pair{true, false});
}

TEST_CASE("a file's identity changes when it is replaced by a rename, not when it is read") {
    namespace fs = std::filesystem;
    const egraph::test::TempDir dir;
    const auto path = dir.path() / "egraph";
    egraph::test::write_text(path, "one");
    const auto first = egraph::os::identity(path);
    REQUIRE(first);
    CHECK(egraph::os::identity(path) == first);
    egraph::test::write_text(dir.path() / "new", "one");
    fs::rename(dir.path() / "new", path);
    const auto second = egraph::os::identity(path);
    REQUIRE(second);
    CHECK(second->inode != first->inode);
    CHECK_FALSE(egraph::os::identity(dir.path() / "missing"));
}

TEST_CASE("this process's command line and executable, as /proc has them") {
    const auto argv = egraph::os::command_line();
    REQUIRE(argv);
    CHECK(std::filesystem::path{argv->front()}.filename() == "unit-tests");
    const auto running = egraph::os::running_identity();
    REQUIRE(running);
    CHECK(egraph::os::identity(egraph::os::executable()) == running);
}

TEST_CASE("appending under the file's lock adds to what is there, and makes it if need be") {
    const egraph::test::TempDir dir;
    const auto path = dir.path() / "a/b/log";
    REQUIRE(egraph::os::append_locked(path, "one\n"));
    REQUIRE(egraph::os::append_locked(path, "two\n"));
    CHECK(egraph::test::read_text(path) == "one\ntwo\n");
    CHECK_FALSE(egraph::os::append_locked(dir.path() / "a/b/log/under", "x"));
}

TEST_CASE("appending by replacing adds to a file this process cannot write, in its directory") {
    namespace fs = std::filesystem;
    const egraph::test::TempDir dir;
    const auto path = dir.path() / "a/history.log";
    REQUIRE(egraph::os::append_replacing(path, "one\n"));
    CHECK((fs::status(path).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
           fs::perms::others_read));
    // As a file another user made would be.
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    REQUIRE(egraph::os::append_replacing(path, "two\n"));
    CHECK(egraph::test::read_text(path) == "one\ntwo\n");
    CHECK(std::distance(fs::directory_iterator{path.parent_path()}, fs::directory_iterator{}) == 1);
    CHECK_FALSE(egraph::os::append_replacing(path / "under", "x"));
}

TEST_CASE("a file replaced with text holds the text alone, with no copy left beside it") {
    const egraph::test::TempDir dir;
    const auto path = dir.path() / "a/b/state.json";
    REQUIRE(egraph::os::replace_with_text(path, "first, and longer\n"));
    REQUIRE(egraph::os::replace_with_text(path, "second\n"));
    CHECK(egraph::test::read_text(path) == "second\n");
    CHECK(std::distance(std::filesystem::directory_iterator{path.parent_path()},
                        std::filesystem::directory_iterator{}) == 1);
    CHECK_FALSE(egraph::os::replace_with_text(path / "under", "x"));
}

TEST_CASE("the journal takes an entry when egraph has it") {
    const std::vector<std::string> fields{"MESSAGE=egraph unit test", "PRIORITY=7",
                                          "SYSLOG_IDENTIFIER=egraph-test"};
    const auto sent = egraph::os::journal_send(fields);
    if (egraph::os::journal_built()) {
        // Without a journal to reach, the socket is missing.
        CHECK((sent || sent.error() == std::errc::no_such_file_or_directory ||
               sent.error() == std::errc::connection_refused));
    } else {
        CHECK(sent.error() == std::errc::function_not_supported);
    }
}
