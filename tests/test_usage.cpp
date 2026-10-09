#include "usage.hpp"

#include "helpers.hpp"
#include "os.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>

namespace {

// A /proc/<pid>/stat line: the comm holds a space and a parenthesis, as one may.
std::string stat_line(std::int64_t pid, std::int64_t ppid, std::uint64_t rss, std::uint64_t cutime,
                      std::uint64_t cstime) {
    return std::format("{} (a) b) S {} 1 1 0 -1 4194560 100 200 0 0 7 8 {} {} 20 0 1 0 500 "
                       "1000000 {} 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 3 0 0\n",
                       pid, ppid, cutime, cstime, rss);
}

constexpr std::string_view io_text = "rchar: 3000\n"
                                     "wchar: 2000\n"
                                     "syscr: 10\n"
                                     "syscw: 5\n"
                                     "read_bytes: 4096\n"
                                     "write_bytes: 12288\n"
                                     "cancelled_write_bytes: 4096\n";

void write_process(const std::filesystem::path& proc, std::int64_t pid, std::int64_t ppid,
                   std::uint64_t rss, std::uint64_t cutime = 0) {
    const auto dir = proc / std::to_string(pid);
    std::filesystem::create_directories(dir);
    egraph::test::write_text(dir / "stat", stat_line(pid, ppid, rss, cutime, 0));
}

} // namespace

TEST_CASE("a process's stat is read after its name, whatever the name holds") {
    const auto stat = egraph::usage::parse_stat(stat_line(42, 7, 300, 150, 50));
    REQUIRE(stat.has_value());
    CHECK(*stat == egraph::usage::Stat{.ppid = 7, .rss_pages = 300, .children_ticks = 200});
    CHECK_FALSE(egraph::usage::parse_stat("42 (a) S 7").has_value());
    CHECK_FALSE(egraph::usage::parse_stat("42 a S 7 1 1 0 -1 0 0 0 0 0 1 1 x 1").has_value());
    CHECK_FALSE(egraph::usage::parse_stat("").has_value());
}

TEST_CASE("a process's IO counts what reached storage, less writes cancelled before") {
    CHECK(egraph::usage::parse_io(io_text) ==
          egraph::usage::Io{.read_bytes = 4096, .write_bytes = 8192});
    CHECK(egraph::usage::parse_io("read_bytes: 1\nwrite_bytes: 2\n") ==
          egraph::usage::Io{.read_bytes = 1, .write_bytes = 2});
    CHECK(egraph::usage::parse_io("read_bytes: 1\nwrite_bytes: 2\ncancelled_write_bytes: 5\n") ==
          egraph::usage::Io{.read_bytes = 1, .write_bytes = 0});
    CHECK_FALSE(egraph::usage::parse_io("rchar: 3000\n").has_value());
    CHECK_FALSE(egraph::usage::parse_io("read_bytes: x\nwrite_bytes: 2\n").has_value());
}

TEST_CASE("a process's counters are its waited-for children's CPU and its IO") {
    const egraph::test::TempDir dir;
    write_process(dir.path(), 10, 1, 50, 250);
    auto counters = egraph::usage::read_counters(10, 100, dir.path());
    REQUIRE(counters.has_value());
    CHECK(counters->children_cpu == 2.5);
    CHECK_FALSE(counters->read_bytes.has_value());

    egraph::test::write_text(dir.path() / "10" / "io", io_text);
    counters = egraph::usage::read_counters(10, 100, dir.path());
    REQUIRE(counters.has_value());
    CHECK(counters->read_bytes == 4096U);
    CHECK(counters->write_bytes == 8192U);

    CHECK_FALSE(egraph::usage::read_counters(11, 100, dir.path()).has_value());
}

TEST_CASE("the memory under a process is its descendants', at any depth, its own left out") {
    const egraph::test::TempDir dir;
    write_process(dir.path(), 1, 0, 5);
    write_process(dir.path(), 10, 1, 1000);
    write_process(dir.path(), 11, 10, 20);
    write_process(dir.path(), 12, 11, 300);
    write_process(dir.path(), 13, 10, 4);
    write_process(dir.path(), 20, 1, 7000);
    // Not a process, and a process ended between listing and reading.
    std::filesystem::create_directories(dir.path() / "self");
    std::filesystem::create_directories(dir.path() / "30");
    egraph::test::write_text(dir.path() / "loadavg", "1 1 1 1/1 1\n");

    const auto processes = egraph::usage::read_processes(dir.path());
    CHECK(processes.size() == 6);
    CHECK(egraph::usage::pages_under(processes, 10) == 20 + 300 + 4);
    CHECK(egraph::usage::pages_under(processes, 12) == 0);
    CHECK(egraph::usage::pages_under(processes, 99) == 0);
    CHECK(egraph::usage::read_processes(dir.path() / "missing").empty());
}

TEST_CASE("this process's counters and tree can be read from the real /proc") {
    const auto counters = egraph::usage::read_counters(egraph::os::process_id(), 100);
    CHECK(counters.has_value());
    CHECK_FALSE(egraph::usage::read_processes().empty());
}
