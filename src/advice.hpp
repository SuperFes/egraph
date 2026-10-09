#pragma once

// Advice on a plan before it runs, from its estimate and the build history: what its tests cost,
// the jobs its builds' memory allows, the builds too large for the tmpfs they build on, and the
// long builds a binary package could replace.

#include "builds.hpp"
#include "estimate.hpp"
#include "exec.hpp"
#include "os.hpp"
#include "repository.hpp"
#include "steve.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// A build's test phase, as its estimate foretells it.
struct TestCost {
    std::string cpv{};
    double seconds = 0;
    bool operator==(const TestCost&) const = default;
};

// A plan's tests are advised on once they take this long in all, naming the costliest
// test_advice_packages of them.
inline constexpr double test_advice_seconds = 120;
inline constexpr std::size_t test_advice_packages = 3;

// A plan's builds are advised on once the memory they would hold at once passes this share of
// the system's.
inline constexpr double memory_advice_share = 0.8;

struct MemoryAdvice {
    // The most the builds running at once would hold, and the system's memory.
    std::uint64_t peak = 0;
    std::uint64_t total = 0;
    // The most jobs that keep the peak within memory_advice_share, and the plan's seconds and
    // peak at them; none when one at a time does not.
    std::optional<std::uint32_t> jobs{};
    double seconds = 0;
    std::uint64_t jobs_peak = 0;
    // The build that holds the most alone, and how much.
    std::string largest{};
    std::uint64_t largest_peak = 0;
    // Where steve is the jobserver and holds its tokens back at less: the MiB of free memory to
    // hold them back at, the share of the system's beyond memory_advice_share.
    std::optional<std::uint64_t> steve_floor{};
    bool operator==(const MemoryAdvice&) const = default;
};

// A build whose directory grew past the size of the tmpfs PORTAGE_TMPDIR is on.
struct TmpfsAdvice {
    std::string cpv{};
    std::uint64_t bytes = 0;
    std::string tmpdir{};
    std::uint64_t size = 0;
    bool operator==(const TmpfsAdvice&) const = default;
};

// A planned build long enough that a binary package matching it is worth naming: binary
// advice is given from this many seconds of building.
inline constexpr double binary_advice_seconds = 600;

// A planned build a binary package could replace.
struct BinaryAdvice {
    std::string cpv{};
    // The building it saves.
    double seconds = 0;
    BinarySourceKind kind = BinarySourceKind::local;
    // PKGDIR, or the binhost's name (its URI without one).
    std::string source{};
    std::uint64_t size = 0;
    bool operator==(const BinaryAdvice&) const = default;
};

// A plan run in emerge's order is told of another that would finish sooner, once that takes no
// more than this share of the time and saves at least order_advice_seconds.
inline constexpr double order_advice_share = 0.9;
inline constexpr double order_advice_seconds = 300;

struct OrderAdvice {
    BuildOrder order = BuildOrder::critical;
    // The plan's seconds in that order, and in emerge's.
    double seconds = 0;
    double emerge_seconds = 0;
    bool operator==(const OrderAdvice&) const = default;
};

struct PlanAdvice {
    // The seconds the builds' tests take, summed, and the costliest of them first; none when
    // they take less than test_advice_seconds.
    double test_seconds = 0;
    std::vector<TestCost> tests{};
    std::optional<MemoryAdvice> memory{};
    std::vector<TmpfsAdvice> tmpfs{};
    std::vector<BinaryAdvice> binaries{};
    std::optional<OrderAdvice> order{};
    bool operator==(const PlanAdvice&) const = default;
};

// The advice on a plan's steps from their costs (by step, as step_costs gives them): its tests.
[[nodiscard]] PlanAdvice plan_advice(std::span<const BuildStep> steps,
                                     std::span<const StepCost> costs);

// What the plan (its steps, and built for their packages) would hold at once at jobs (none for
// a token a core) on a system of cores and total bytes of memory, as estimated: none within
// memory_advice_share, else the most jobs below that, found by estimating the plan again at
// fewer, and the build holding the most alone.
[[nodiscard]] std::optional<MemoryAdvice>
memory_advice(const Plan& plan, std::span<const Step> steps, std::span<const BuildStep> built,
              std::span<const StepCost> costs, unsigned cores, std::optional<std::uint32_t> jobs,
              const PlanEstimate& estimate, std::uint64_t total);

// The free MiB steve should hold its tokens back below on a system of total bytes, to keep its
// builds within memory_advice_share; none when its settings hold them back at that or more.
[[nodiscard]] std::optional<std::uint64_t> steve_floor(std::uint64_t total,
                                                       const steve::Settings& settings);

// The PORTAGE_TMPDIRs the steps' packages last built in, each once, to look their filesystems
// up.
[[nodiscard]] std::vector<std::string> build_tmpdirs(std::span<const StepCost> costs);

// The steps (by step, built for their packages) whose packages' build directories grew past the
// size of the tmpfs their PORTAGE_TMPDIR is on (its space in spaces), where they build with the
// env files they did then: a new one may move them.
[[nodiscard]] std::vector<TmpfsAdvice>
tmpfs_advice(std::span<const BuildStep> built, std::span<const StepCost> costs,
             const std::map<std::string, os::FilesystemSpace, std::less<>>& spaces);

// Whether emerge would take binary for step under --binpkg-respect-use, as depgraph's
// _reinstall_for_flags decides it (as --newuse does): no flag of either IUSE missing from the
// other but those the profile masks or forces, and the same of the IUSE enabled.
[[nodiscard]] bool binary_use_matches(const RepositoryIndex& index, const Binary& binary,
                                      const BuildStep& step);

// The steps (by step, built for their packages, costs as step_costs gives them) of
// binary_advice_seconds or more of building that a binary in index could replace: the first of
// their cpv, by source, that nothing refuses and whose USE matches.
[[nodiscard]] std::vector<BinaryAdvice> binary_advice(const RepositoryIndex& index,
                                                      std::span<const BuildStep> built,
                                                      std::span<const StepCost> costs);

// The order other than emerge's that would run the plan (its steps, at jobs, none for a token a
// core, on a system of cores) soonest, as estimated, where it beats estimate, the plan's in
// emerge's order, by order_advice_share and order_advice_seconds; none otherwise.
[[nodiscard]] std::optional<OrderAdvice>
order_advice(const Plan& plan, std::span<const Step> steps, std::span<const StepCost> costs,
             unsigned cores, std::optional<std::uint32_t> jobs, const PlanEstimate& estimate);

// For scripts, after a plan's estimate line, where advised: "<TAB>advice<TAB>tests<TAB>seconds",
// then "<TAB>advice<TAB>test<TAB>cpv<TAB>seconds" for each package named, seconds whole;
// "<TAB>advice<TAB>memory<TAB>peak<TAB>total<TAB>jobs<TAB>seconds<TAB>jobs peak<TAB>largest
// cpv<TAB>its peak<TAB>steve floor", bytes but the floor's MiB, jobs, its seconds and peak empty
// when there are none, as is the floor; and "<TAB>advice<TAB>tmpfs<TAB>cpv<TAB>bytes<TAB>tmpdir
// <TAB>its size" for each build too large for its tmpfs; and "<TAB>advice<TAB>binary<TAB>cpv
// <TAB>seconds<TAB>local or binhost<TAB>source<TAB>bytes" for each build a binary could replace,
// seconds whole; and "<TAB>advice<TAB>order<TAB>name<TAB>seconds<TAB>emerge's seconds", seconds
// whole, where another order would finish sooner.
[[nodiscard]] std::vector<std::string> advice_lines(const PlanAdvice& advice);

// Whether a plan's line is one of advice_lines'.
[[nodiscard]] bool is_advice_line(std::string_view line);

} // namespace egraph
