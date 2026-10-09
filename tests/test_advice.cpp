#include "advice.hpp"

#include "index_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

using egraph::BuildStep;
using egraph::MemoryAdvice;
using egraph::PlanAdvice;
using egraph::StepCost;
using egraph::TestCost;

namespace {

constexpr std::uint64_t gib = std::uint64_t{1} << 30U;

// A plan of merges no merge waits for, and its steps and packages.
struct Planned {
    egraph::Plan plan;
    std::vector<egraph::Step> steps;
    std::vector<BuildStep> built;
};

Planned apart(std::size_t merges) {
    Planned found;
    for (std::uint32_t i = 0; i < merges; ++i) {
        found.plan.merges.emplace_back();
        found.plan.order.push_back(i);
        found.steps.emplace_back(egraph::MergeStep{.merge = i, .blockers = {}, .world = {}});
        found.built.push_back({.cpv = std::format("app-misc/p{}-1", i)});
    }
    return found;
}

StepCost holding(std::uint64_t memory) {
    return {.build_seconds = 100,
            .parallelism = 1,
            .peak_memory = memory,
            .merge_seconds = 1,
            .known = true};
}

StepCost tested(double seconds, double test) {
    return {.build_seconds = seconds, .known = true, .test_seconds = test};
}

} // namespace

TEST_CASE("a plan's tests are advised on once they cost enough, the costliest named") {
    const std::vector<BuildStep> steps{
        {.cpv = "app-misc/a-1"}, {.cpv = "app-misc/b-1"},
        {.cpv = "app-misc/c-1"}, {.cpv = "app-misc/d-1"},
        {.cpv = "app-misc/e-1"}, {.cpv = "app-misc/a-0", .uninstall = true}};
    const std::vector<StepCost> costs{tested(100, 10), tested(500, 90), {},
                                      tested(50, 0),   tested(300, 40), tested(5, 3)};
    CHECK(egraph::plan_advice(steps, costs) ==
          PlanAdvice{.test_seconds = 140,
                     .tests = {{.cpv = "app-misc/b-1", .seconds = 90},
                               {.cpv = "app-misc/e-1", .seconds = 40},
                               {.cpv = "app-misc/a-1", .seconds = 10}}});
    // Too little to say.
    const std::vector<StepCost> cheap{tested(100, 10), tested(500, 90), {}, {}, {}, {}};
    CHECK(egraph::plan_advice(steps, cheap) == PlanAdvice{});
    CHECK(egraph::plan_advice({}, {}) == PlanAdvice{});
}

TEST_CASE("a plan's advice is lines for scripts") {
    const PlanAdvice advice{.test_seconds = 140.4,
                            .tests = {{.cpv = "app-misc/b-1", .seconds = 90.6},
                                      {.cpv = "app-misc/e-1", .seconds = 40}}};
    const auto lines = egraph::advice_lines(advice);
    CHECK(lines == std::vector<std::string>{"\tadvice\ttests\t140",
                                            "\tadvice\ttest\tapp-misc/b-1\t91",
                                            "\tadvice\ttest\tapp-misc/e-1\t40"});
    for (const auto& line : lines) {
        CHECK(egraph::is_advice_line(line));
    }
    CHECK(egraph::advice_lines(PlanAdvice{}).empty());
    CHECK_FALSE(egraph::is_advice_line("\testimate\t10\t\t1\t0\t\t0"));
    CHECK_FALSE(egraph::is_advice_line("app-misc/a\t1\t2"));
}

TEST_CASE("a plan whose builds would hold most of the memory is told the jobs that fit") {
    const auto [plan, steps, built] = apart(4);
    const std::vector<StepCost> costs{holding(10 * gib), holding(10 * gib), holding(10 * gib),
                                      holding(12 * gib)};
    const auto at = [&](std::optional<std::uint32_t> jobs) {
        return egraph::estimate_plan(egraph::Schedule{plan, steps, jobs}, costs, 8,
                                     jobs ? std::nullopt : std::optional<std::uint32_t>{8});
    };
    // A token a core: all four at once, 42 GiB of 32.
    const auto unlimited = at(std::nullopt);
    REQUIRE(unlimited.peak_memory == 42 * gib);
    const auto fewer = at(2);
    CHECK(egraph::memory_advice(plan, steps, built, costs, 8, std::nullopt, unlimited, 32 * gib) ==
          MemoryAdvice{.peak = 42 * gib,
                       .total = 32 * gib,
                       .jobs = 2,
                       .seconds = fewer.seconds,
                       .jobs_peak = fewer.peak_memory.value_or(0),
                       .largest = "app-misc/p3-1",
                       .largest_peak = 12 * gib});
    // At -j3 too, the same.
    CHECK(egraph::memory_advice(plan, steps, built, costs, 8, 3, at(3), 32 * gib)->jobs == 2U);
    // Within the share, nothing to say; nor without the peaks.
    CHECK_FALSE(egraph::memory_advice(plan, steps, built, costs, 8, 2, fewer, 32 * gib));
    CHECK_FALSE(
        egraph::memory_advice(plan, steps, built, costs, 8, std::nullopt, unlimited, 64 * gib));
    CHECK_FALSE(egraph::memory_advice(plan, steps, built, std::vector<StepCost>(4), 8, std::nullopt,
                                      egraph::PlanEstimate{}, 32 * gib));
}

TEST_CASE("a build too large alone is named, with no jobs to fit") {
    const auto [plan, steps, built] = apart(2);
    const std::vector<StepCost> costs{holding(30 * gib), holding(1 * gib)};
    const auto estimate = egraph::estimate_plan(egraph::Schedule{plan, steps, 1}, costs, 8);
    CHECK(egraph::memory_advice(plan, steps, built, costs, 8, 1, estimate, 32 * gib) ==
          MemoryAdvice{.peak = 30 * gib,
                       .total = 32 * gib,
                       .largest = "app-misc/p0-1",
                       .largest_peak = 30 * gib});
}

TEST_CASE("steve is told the free memory to hold its tokens back below") {
    // A fifth of 32 GiB, in MiB, rounded up.
    CHECK(egraph::steve_floor(32 * gib, {}) == 6554U);
    CHECK(egraph::steve_floor(32 * gib, {.min_memory = 1000}) == 6554U);
    CHECK_FALSE(egraph::steve_floor(32 * gib, {.min_memory = 6554}));
    CHECK_FALSE(egraph::steve_floor(32 * gib, {.min_memory = 8000}));
}

TEST_CASE("a plan's memory advice is a line for scripts") {
    PlanAdvice advice{.memory = MemoryAdvice{.peak = 42,
                                             .total = 32,
                                             .jobs = 2,
                                             .seconds = 201.4,
                                             .jobs_peak = 22,
                                             .largest = "app-misc/p3-1",
                                             .largest_peak = 12,
                                             .steve_floor = 7}};
    CHECK(egraph::advice_lines(advice) ==
          std::vector<std::string>{"\tadvice\tmemory\t42\t32\t2\t201\t22\tapp-misc/p3-1\t12\t7"});
    advice.memory =
        MemoryAdvice{.peak = 30, .total = 32, .largest = "app-misc/p0-1", .largest_peak = 30};
    CHECK(egraph::advice_lines(advice) ==
          std::vector<std::string>{"\tadvice\tmemory\t30\t32\t\t\t\tapp-misc/p0-1\t30\t"});
}

TEST_CASE("a build too large for the tmpfs it builds on is offered the disk") {
    const auto ran = [](std::uint64_t bytes, std::string tmpdir,
                        std::vector<std::string> env = {}) {
        return StepCost{.build_seconds = 100,
                        .known = true,
                        .build_directory = egraph::BuildDirectory{.bytes = bytes,
                                                                  .tmpdir = std::move(tmpdir),
                                                                  .env_files = std::move(env)}};
    };
    const std::vector<BuildStep> built{
        {.cpv = "app-misc/big-1"},     {.cpv = "app-misc/small-1"},
        {.cpv = "app-misc/disk-1"},    {.cpv = "app-misc/moved-1", .env_files = {"/env/notmpfs"}},
        {.cpv = "app-misc/unknown-1"}, {.cpv = "app-misc/gone-1", .uninstall = true}};
    const std::vector<StepCost> costs{ran(9 * gib, "/var/tmp"),
                                      ran(1 * gib, "/var/tmp"),
                                      ran(20 * gib, "/var/tmp/disk"),
                                      ran(9 * gib, "/var/tmp"),
                                      {},
                                      ran(9 * gib, "/var/tmp")};
    CHECK(egraph::build_tmpdirs(costs) == std::vector<std::string>{"/var/tmp", "/var/tmp/disk"});
    const std::map<std::string, egraph::os::FilesystemSpace, std::less<>> spaces{
        {"/var/tmp", {.size = 8 * gib, .free = 8 * gib, .tmpfs = true}},
        {"/var/tmp/disk", {.size = 10 * gib, .free = 10 * gib, .tmpfs = false}}};
    CHECK(egraph::tmpfs_advice(built, costs, spaces) ==
          std::vector<egraph::TmpfsAdvice>{
              {.cpv = "app-misc/big-1", .bytes = 9 * gib, .tmpdir = "/var/tmp", .size = 8 * gib}});
    // A tmpdir not looked up says nothing.
    CHECK(egraph::tmpfs_advice(built, costs, {}).empty());
    const auto lines = egraph::advice_lines(PlanAdvice{
        .tmpfs = {{.cpv = "app-misc/big-1", .bytes = 9, .tmpdir = "/var/tmp", .size = 8}}});
    CHECK(lines == std::vector<std::string>{"\tadvice\ttmpfs\tapp-misc/big-1\t9\t/var/tmp\t8"});
}

namespace {

std::vector<std::string> flags(std::string_view text) {
    return egraph::test::words(text);
}

// A merge of cpv built with use, of iuse, forced those of it the profile masks or forces.
BuildStep merging(std::string cpv, std::string_view use, std::string_view iuse,
                  std::string_view forced = {}) {
    return {.cpv = std::move(cpv), .use = flags(use), .iuse = flags(iuse), .forced = flags(forced)};
}

} // namespace

TEST_CASE("a binary's USE matches as binpkg-respect-use matches it") {
    egraph::test::IndexBuilder builder;
    builder.source(egraph::BinarySourceKind::local, "", "/var/cache/binpkgs");
    const auto binary = [&](std::string_view use, std::string_view iuse) {
        builder.binary({.cpv = "app-misc/a-1", .use = std::string{use}, .iuse = std::string{iuse}});
        return builder.index().binaries.back();
    };
    const auto step = merging("app-misc/a-1", "amd64 bar foo", "bar foo");
    // Flags outside IUSE, as the arch, are not compared.
    CHECK(egraph::binary_use_matches(builder.index(), binary("bar elibc_glibc foo", "bar foo"),
                                     step));
    CHECK_FALSE(egraph::binary_use_matches(builder.index(), binary("bar", "bar foo"), step));
    // A flag the ebuild's IUSE lacks, though disabled.
    CHECK_FALSE(
        egraph::binary_use_matches(builder.index(), binary("bar foo", "bar extra foo"), step));
    // One the profile masks is no difference while disabled in both.
    const auto masked = merging("app-misc/a-1", "bar foo", "bar extra foo", "extra");
    CHECK(egraph::binary_use_matches(builder.index(), binary("bar foo", "bar foo"), masked));
    // One it forces still differs in what is enabled.
    const auto forced = merging("app-misc/a-1", "bar extra foo", "bar extra foo", "extra");
    CHECK_FALSE(egraph::binary_use_matches(builder.index(), binary("bar foo", "bar foo"), forced));
}

TEST_CASE("a long build a binary could replace is named, with the binary's source") {
    egraph::test::IndexBuilder builder;
    builder.source(egraph::BinarySourceKind::local, "", "/var/cache/binpkgs");
    builder.source(egraph::BinarySourceKind::binhost, "gentoo", "https://binhost/amd64");
    builder.source(egraph::BinarySourceKind::binhost, "", "https://other/amd64");
    // a's local binary was built without foo; the binhost's matches.
    builder.binary({.cpv = "app-misc/a-1", .use = "bar", .iuse = "bar foo", .source = 0});
    builder.binary(
        {.cpv = "app-misc/a-1", .use = "bar foo", .iuse = "bar foo", .source = 1, .size = 100});
    // b's deps changed since.
    builder.binary({.cpv = "app-misc/b-1", .source = 0, .ignored = egraph::binary_changed_deps});
    builder.binary({.cpv = "app-misc/c-1", .source = 0});
    builder.binary({.cpv = "app-misc/d-1", .source = 0, .size = 7});
    builder.binary({.cpv = "app-misc/e-1", .source = 0});
    builder.binary({.cpv = "app-misc/f-1", .source = 2, .size = 9});
    const std::vector<BuildStep> built{merging("app-misc/a-1", "bar foo", "bar foo"),
                                       merging("app-misc/b-1", "", ""),
                                       merging("app-misc/c-1", "", ""),
                                       merging("app-misc/d-1", "", ""),
                                       {.cpv = "app-misc/e-1", .uninstall = true},
                                       merging("app-misc/f-1", "", ""),
                                       merging("app-misc/g-1", "", "")};
    const std::vector<StepCost> costs{tested(900, 0), tested(900, 0), tested(100, 0),
                                      tested(600, 0), tested(900, 0), tested(700, 0),
                                      tested(900, 0)};
    using egraph::BinaryAdvice;
    using egraph::BinarySourceKind;
    CHECK(egraph::binary_advice(builder.index(), built, costs) ==
          std::vector<BinaryAdvice>{{.cpv = "app-misc/a-1",
                                     .seconds = 900,
                                     .kind = BinarySourceKind::binhost,
                                     .source = "gentoo",
                                     .size = 100},
                                    {.cpv = "app-misc/d-1",
                                     .seconds = 600,
                                     .kind = BinarySourceKind::local,
                                     .source = "/var/cache/binpkgs",
                                     .size = 7},
                                    {.cpv = "app-misc/f-1",
                                     .seconds = 700,
                                     .kind = BinarySourceKind::binhost,
                                     .source = "https://other/amd64",
                                     .size = 9}});
    CHECK(egraph::advice_lines(PlanAdvice{.binaries = {{.cpv = "app-misc/a-1",
                                                        .seconds = 900.4,
                                                        .kind = BinarySourceKind::binhost,
                                                        .source = "gentoo",
                                                        .size = 100},
                                                       {.cpv = "app-misc/d-1",
                                                        .seconds = 600,
                                                        .kind = BinarySourceKind::local,
                                                        .source = "/var/cache/binpkgs",
                                                        .size = 7}}}) ==
          std::vector<std::string>{"\tadvice\tbinary\tapp-misc/a-1\t900\tbinhost\tgentoo\t100",
                                   "\tadvice\tbinary\tapp-misc/d-1\t600\tlocal\t/var/cache/"
                                   "binpkgs\t7"});
}
