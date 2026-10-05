#pragma once

// Which merges emerge's scheduler holds back to merge alone, once no build runs, whatever
// FEATURES=merge-wait says: the merge-wait scope (the fork's --merge-wait-scope), as
// _find_deep_system_runtime_deps finds it.

#include "evaluated.hpp"
#include "exec.hpp"
#include "graph.hpp"
#include "plan.hpp"
#include "store.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace egraph {

enum class MergeWaitScope : std::uint8_t { deep, system, toolchain, none };

// The scope a --merge-wait-scope value names.
[[nodiscard]] std::optional<MergeWaitScope> merge_wait_scope(std::string_view name);

// The fork's CORE_TOOLCHAIN: what the ebuild environment runs for nearly every build, so that a
// half merged one can break an unrelated build running beside it.
inline constexpr std::array<std::string_view, 12> core_toolchain{
    "app-shells/bash",     "dev-lang/perl",      "dev-lang/python",  "dev-libs/libffi",
    "sys-apps/baselayout", "sys-apps/coreutils", "sys-apps/sandbox", "sys-devel/binutils",
    "sys-devel/gcc",       "sys-devel/gettext",  "sys-libs/glibc",   "sys-libs/musl"};

// Per step, whether it merges alone under scope: toolchain, the merges of core_toolchain's
// packages; system, the merges @system's atoms match; deep, those and what they reach through
// run-time dependencies (waits by RDEPEND or PDEPEND, and through installed packages), and what
// installed @system members that stay reach through theirs. Uninstalls never. Emerge's graph
// holds an installed package only when its resolution reached it, so deep may hold more than
// emerge would, never less.
[[nodiscard]] std::vector<bool> merge_wait_steps(const Store& store, const Graph& graph,
                                                 const Evaluated& evaluated, const Plan& plan,
                                                 std::span<const Step> steps, MergeWaitScope scope);

// The merges a merge of plan waits for at run time (RDEPEND or PDEPEND): what emerge counts as
// still unsatisfied once that merge has started, if they have not merged yet.
[[nodiscard]] std::vector<std::uint32_t> run_time_waits(const Plan& plan, std::uint32_t merge);

} // namespace egraph
