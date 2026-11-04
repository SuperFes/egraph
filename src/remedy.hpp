#pragma once

#include "atom.hpp"
#include "evaluated.hpp"
#include "graph.hpp"
#include "plan.hpp"
#include "query.hpp"
#include "store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace egraph {

// An installed package whose dependencies, or those of its rebuild, reject a held update.
struct Holder {
    std::uint32_t package = 0;
    // The other installed packages depending on it, the update's other holders aside; by id.
    std::vector<std::uint32_t> dependents;
    // Indices into Store::roots of the root atoms that select it.
    std::vector<std::uint32_t> roots;
};

// The ways past one held update.
struct Remedy {
    // Index into Plan::held.
    std::uint32_t held = 0;
    // The installed packages among what rejects it, by id.
    std::vector<Holder> holders;
    // Only holders reject it, not its own dependencies, so emerge --nodeps would merge it.
    bool nodeps = false;
    // Removing every holder lets the update through: each is a leaf (no dependents, and no root
    // set but @selected selects it), and the plan without them merges the wanted version.
    bool removable = false;
    // With removable, the other held updates the removal lets through, as indices into
    // Plan::held.
    std::vector<std::uint32_t> frees;
};

// Whether a holder can go: nothing depends on it and at most @selected keeps it.
[[nodiscard]] bool leaf(const Store& store, const Holder& holder);

// A remedy per held update of plan, which plan_updates made for targets. rescope gives the
// scope once the holders are gone; unset, the targets' scope without them.
[[nodiscard]] std::vector<Remedy> remedies(const Store& store, const Evaluated& evaluated,
                                           const Graph& graph, const Plan& plan,
                                           UseRebuilds rebuilds, const Targets& targets,
                                           const Rescope& rescope = {});

// What keeps an unsatisfied atom from being met: the installed package whose version it would
// replace, and the installed packages whose own atoms keep that version, with the atom each.
struct Conflict {
    std::uint32_t installed = 0;
    std::vector<std::pair<std::uint32_t, std::string>> holders;
};

// For each of plan.unsatisfied, its conflict; none where no installed version stands in its way
// (no ebuild matches, or the packages of its name are not installed), or no installed package
// holds it.
[[nodiscard]] std::vector<std::optional<Conflict>>
conflicts(const Store& store, const Evaluated& evaluated, const Graph& graph, const Plan& plan);

} // namespace egraph
