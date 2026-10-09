#include "cgroup.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("a flat keyed cgroup file is read by key", "[cgroup]") {
    constexpr std::string_view cpu_stat = "usage_usec 2500000\n"
                                          "user_usec 2000000\n"
                                          "system_usec 500000\n"
                                          "nr_periods 0\n";
    CHECK(egraph::cgroup::parse_keyed(cpu_stat, "usage_usec") == 2500000U);
    CHECK(egraph::cgroup::parse_keyed(cpu_stat, "system_usec") == 500000U);
    CHECK_FALSE(egraph::cgroup::parse_keyed(cpu_stat, "usage").has_value());
    CHECK_FALSE(egraph::cgroup::parse_keyed("usage_usec many\n", "usage_usec").has_value());
}

TEST_CASE("io.stat is summed over its devices", "[cgroup]") {
    constexpr std::string_view io_stat =
        "8:0 rbytes=4096 wbytes=8192 rios=1 wios=2 dbytes=0 dios=0\n"
        "259:0 rbytes=1000 wbytes=24 rios=3 wios=1 dbytes=0 dios=0\n";
    CHECK(egraph::cgroup::parse_io_stat(io_stat) ==
          egraph::cgroup::Io{.read_bytes = 5096, .write_bytes = 8216});
    // A leaf that has done no IO lists no device.
    CHECK(egraph::cgroup::parse_io_stat("") == egraph::cgroup::Io{});
    CHECK_FALSE(egraph::cgroup::parse_io_stat("8:0 rbytes=x wbytes=1\n").has_value());
}

TEST_CASE("a leaf is named for its cpv as portage names it", "[cgroup]") {
    CHECK(egraph::cgroup::leaf_name("dev-libs/foo-1.2_rc3-r1") == "dev-libs_foo-1.2_rc3-r1");
    CHECK(egraph::cgroup::leaf_name("dev-cpp/gtk+-3") == "dev-cpp_gtk+-3");
    CHECK(egraph::cgroup::leaf_name("a/b c:d") == "a_b_c_d");
}

TEST_CASE("a leaf's counters are read from its interface files", "[cgroup]") {
    const egraph::test::TempDir dir;
    const auto leaf = dir.path() / "app-misc_foo-1";
    CHECK_FALSE(egraph::cgroup::read_counters(leaf).has_value());
    std::filesystem::create_directories(leaf);
    // A kernel without the controllers, or without PSI, shows only the core files.
    CHECK(egraph::cgroup::read_counters(leaf) == egraph::cgroup::Counters{});
    egraph::test::write_text(leaf / "cpu.stat", "usage_usec 1500000\nuser_usec 1000000\n");
    egraph::test::write_text(leaf / "memory.peak", "104857600\n");
    egraph::test::write_text(leaf / "io.stat", "8:0 rbytes=10 wbytes=20 rios=1 wios=1\n");
    egraph::test::write_text(leaf / "cpu.pressure", "some avg10=0.00 avg60=0.00 avg300=0.00 "
                                                    "total=300000\nfull avg10=0.00 avg60=0.00 "
                                                    "avg300=0.00 total=100000\n");
    egraph::test::write_text(leaf / "memory.pressure",
                             "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    CHECK(egraph::cgroup::read_counters(leaf) ==
          egraph::cgroup::Counters{.cpu_usec = 1500000,
                                   .memory_peak = 104857600,
                                   .read_bytes = 10,
                                   .write_bytes = 20,
                                   .stalls = {.cpu = 300000, .memory = 0}});
    // memory.peak may read "max" nowhere, but a value it cannot read is left out.
    egraph::test::write_text(leaf / "memory.peak", "max\n");
    CHECK_FALSE(egraph::cgroup::read_counters(leaf)->memory_peak.has_value());
}

TEST_CASE("a hierarchy is made with its leaves' controllers, and stale ones are reaped",
          "[cgroup]") {
    const egraph::test::TempDir dir;
    const auto fs = dir.path() / "cgroup";
    const auto proc = dir.path() / "proc";
    std::filesystem::create_directories(proc / "4242");
    std::filesystem::create_directories(fs);
    CHECK_FALSE(egraph::cgroup::Hierarchy::create(fs, 77, proc).has_value());

    egraph::test::write_text(fs / "cgroup.controllers", "cpuset cpu io memory pids\n");
    // The kernel fills a new cgroup's interface files; here egraph's own is made ahead.
    std::filesystem::create_directories(fs / "egraph" / "exec-4242");
    std::filesystem::create_directories(fs / "egraph" / "exec-999999");
    egraph::test::write_text(fs / "egraph" / "cgroup.controllers", "cpu io memory pids\n");
    {
        const auto made = egraph::cgroup::Hierarchy::create(fs, 77, proc);
        REQUIRE(made.has_value());
        CHECK(made->root() == fs / "egraph" / "exec-77");
        CHECK(std::filesystem::is_directory(made->root()));
        CHECK(made->leaf("dev-libs/foo-1") == fs / "egraph" / "exec-77" / "dev-libs_foo-1");
        CHECK(read_file(fs / "cgroup.subtree_control") == "+cpu +io +memory");
        CHECK(read_file(fs / "egraph" / "cgroup.subtree_control") == "+cpu +io +memory");
        // Its own has no controllers to give in a plain directory.
        CHECK_FALSE(std::filesystem::exists(made->root() / "cgroup.subtree_control"));
        // A run whose process is gone left its hierarchy; a running one's is kept.
        CHECK_FALSE(std::filesystem::exists(fs / "egraph" / "exec-999999"));
        CHECK(std::filesystem::is_directory(fs / "egraph" / "exec-4242"));
        std::filesystem::create_directories(made->leaf("dev-libs/foo-1"));
    }
    CHECK_FALSE(std::filesystem::exists(fs / "egraph" / "exec-77"));
}

TEST_CASE("removing a cgroup kills what is left in it first", "[cgroup]") {
    const egraph::test::TempDir dir;
    const auto cgroup = dir.path() / "exec-1";
    std::filesystem::create_directories(cgroup / "leaf");
    egraph::test::write_text(cgroup / "leaf" / "cgroup.kill", "");
    egraph::cgroup::remove(cgroup);
    CHECK(read_file(cgroup / "leaf" / "cgroup.kill") == "1");
    // A plain directory keeps the file, so nothing is removed and nothing throws.
    CHECK(std::filesystem::exists(cgroup / "leaf"));

    std::filesystem::remove(cgroup / "leaf" / "cgroup.kill");
    egraph::cgroup::remove(cgroup);
    CHECK_FALSE(std::filesystem::exists(cgroup));
    // Nor does a cgroup already gone.
    egraph::cgroup::remove(cgroup);
}
