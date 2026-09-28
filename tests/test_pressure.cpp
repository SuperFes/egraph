#include "pressure.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>

namespace {

constexpr std::string_view stat_text = "cpu  100 20 30 800 40 5 5 0 0 0\n"
                                       "cpu0 50 10 15 400 20 2 3 0 0 0\n"
                                       "cpu1 50 10 15 400 20 3 2 0 0 0\n"
                                       "intr 12345 0 0\n"
                                       "ctxt 999\n";

constexpr std::string_view meminfo_text = "MemTotal:       32012848 kB\n"
                                          "MemFree:         1000000 kB\n"
                                          "MemAvailable:   11339588 kB\n";

constexpr std::string_view psi_text = "some avg10=12.50 avg60=3.00 avg300=1.00 total=123\n"
                                      "full avg10=2.00 avg60=0.00 avg300=0.00 total=45\n";

// A /proc with the given CPU times on the aggregate line, and PSI only where asked.
void write_proc(const std::filesystem::path& proc, std::uint64_t user, std::uint64_t idle,
                bool psi) {
    std::filesystem::create_directories(proc);
    egraph::test::write_text(proc / "stat", std::format("cpu  {} 0 0 {} 0 0 0 0 0 0\n"
                                                        "cpu0 0 0 0 0 0 0 0 0 0 0\n",
                                                        user, idle));
    egraph::test::write_text(proc / "meminfo", meminfo_text);
    egraph::test::write_text(proc / "loadavg", "3.55 4.32 5.05 7/3274 1261183\n");
    if (psi) {
        std::filesystem::create_directories(proc / "pressure");
        egraph::test::write_text(proc / "pressure" / "cpu", psi_text);
        egraph::test::write_text(proc / "pressure" / "memory",
                                 "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        egraph::test::write_text(proc / "pressure" / "io",
                                 "some avg10=3.25 avg60=0.00 avg300=0.00 total=0\n");
    }
}

} // namespace

TEST_CASE("proc files are read field by field") {
    const auto times = egraph::pressure::parse_stat(stat_text);
    REQUIRE(times.has_value());
    // Busy is everything but idle and iowait.
    CHECK(times->busy == 100 + 20 + 30 + 5 + 5);
    CHECK(times->total == 1000);
    CHECK(egraph::pressure::count_cpus(stat_text) == 2);
    CHECK_FALSE(egraph::pressure::parse_stat("cpu0 1 2 3 4\n").has_value());
    CHECK_FALSE(egraph::pressure::parse_stat("cpu  1 x 3 4\n").has_value());

    CHECK(egraph::pressure::parse_meminfo(meminfo_text, "MemTotal") == 32012848ULL * 1024);
    CHECK(egraph::pressure::parse_meminfo(meminfo_text, "MemAvailable") == 11339588ULL * 1024);
    CHECK_FALSE(egraph::pressure::parse_meminfo(meminfo_text, "Mem").has_value());
    CHECK_FALSE(egraph::pressure::parse_meminfo(meminfo_text, "SwapTotal").has_value());

    CHECK(egraph::pressure::parse_loadavg("3.55 4.32 5.05 7/3274 1261183\n") == 3.55);
    CHECK_FALSE(egraph::pressure::parse_loadavg("").has_value());

    CHECK(egraph::pressure::parse_psi(psi_text) == 12.5);
    CHECK_FALSE(egraph::pressure::parse_psi("full avg10=2.00\n").has_value());
}

TEST_CASE("a sample reads every file, and PSI only where the kernel keeps it") {
    const egraph::test::TempDir dir;
    write_proc(dir.path(), 10, 90, false);
    const auto without = egraph::pressure::read_sample(dir.path());
    REQUIRE(without.cpu.has_value());
    CHECK(without.cpu->busy == 10);
    CHECK(without.cpus == 1);
    CHECK(without.mem_available == 11339588ULL * 1024);
    CHECK(without.load == 3.55);
    CHECK_FALSE(without.stalls.cpu.has_value());

    write_proc(dir.path(), 10, 90, true);
    const auto with = egraph::pressure::read_sample(dir.path());
    CHECK(with.stalls.cpu == 12.5);
    CHECK(with.stalls.memory == 0.0);
    CHECK(with.stalls.io == 3.25);

    const auto nothing = egraph::pressure::read_sample(dir.path() / "missing");
    CHECK_FALSE(nothing.cpu.has_value());
    CHECK_FALSE(nothing.load.has_value());
}

TEST_CASE("CPU use is measured between samples") {
    const egraph::pressure::Sample before{
        .cpu = egraph::pressure::CpuTimes{.busy = 100, .total = 1000}};
    const egraph::pressure::Sample after{
        .cpu = egraph::pressure::CpuTimes{.busy = 175, .total = 1100}};
    CHECK(egraph::pressure::cpu_busy(before, after) == 0.75);
    // No time passed, or a counter went backwards: nothing to measure.
    CHECK_FALSE(egraph::pressure::cpu_busy(after, after).has_value());
    CHECK_FALSE(egraph::pressure::cpu_busy(after, before).has_value());
    CHECK_FALSE(egraph::pressure::cpu_busy({}, after).has_value());
}

TEST_CASE("history keeps the last readings, CPU measured from the one before") {
    egraph::pressure::History history{3};
    for (std::uint64_t tick = 0; tick < 5; ++tick) {
        history.add({.cpu = egraph::pressure::CpuTimes{.busy = tick * 50, .total = tick * 100},
                     .cpus = 1,
                     .mem_available = tick,
                     .load = static_cast<double>(tick)});
    }
    const auto& readings = history.readings();
    REQUIRE(readings.size() == 3);
    CHECK(readings.front().load == 2.0);
    CHECK(readings.back().mem_available == 4);
    CHECK(readings.back().cpu_busy == 0.5);
    REQUIRE(history.latest().has_value());
    CHECK(history.latest()->load == 4.0);

    // The first reading has nothing to measure CPU against.
    egraph::pressure::History fresh;
    fresh.add({.cpu = egraph::pressure::CpuTimes{.busy = 1, .total = 2}});
    REQUIRE(fresh.readings().size() == 1);
    CHECK_FALSE(fresh.readings().front().cpu_busy.has_value());
}
