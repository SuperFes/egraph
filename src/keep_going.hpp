#pragma once

// What emerge --keep-going leaves out of a run once a step has failed.

#include "evaluated.hpp"
#include "exec.hpp"
#include "plan.hpp"
#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// Where a step stands once a run has drained after a failure.
enum class Standing : std::uint8_t {
    left,
    done,
    // Failed, or left out before.
    gone
};

// A step left out.
struct Skip {
    std::size_t step = 0;
    // Its dependencies nothing satisfies once the steps gone are dropped, as its dependencies
    // print them, or those by which it depends on another step left out; none for an uninstall
    // no merge left needs.
    std::vector<std::string> atoms;
    auto operator<=>(const Skip&) const = default;
};

struct Resumption {
    // In the steps' order.
    std::vector<Skip> skipped;
    // Why the run cannot go on, as emerge refuses to resume when what it would drop is already
    // installed: "cpv: atom" for the first dependency nothing satisfies. Nothing is skipped then.
    std::optional<std::string> stuck;
};

// The steps left that emerge --keep-going drops once those gone are, as its resume depgraph does:
// the merges left, with what is installed now (what the steps done merged, but for what they
// replaced or uninstalled) where no merge left replaces it by slot or cpv, satisfy each merge's
// dependencies, and the run-time dependencies of each installed package a merge reaches through
// them, a || by any of its members. Whatever has one left unsatisfied goes, unless something
// installed matches the atom, and with it whatever reaches it, through the atom it was reached
// by, again unless something installed matches that; then again with the merges left, until
// nothing more goes. An uninstall goes once none of the merges it waits for is done or left.
// Stuck when a pass finds something unsatisfied but drops no merge.
[[nodiscard]] Resumption keep_going(const Store& store, const Evaluated& evaluated,
                                    const Plan& plan, std::span<const Step> steps,
                                    std::span<const Standing> standing);

// Why a step is left out, for a person: "needs a, b" or "no merge left needs it gone".
[[nodiscard]] std::string describe_skip(const Skip& skip);

} // namespace egraph
