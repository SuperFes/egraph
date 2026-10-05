#include "merge_wait.hpp"

#include "atom.hpp"

#include <algorithm>
#include <variant>

namespace egraph {

namespace {

bool run_time(const WaitKinds& kinds) {
    return kinds.run || kinds.post || kinds.through;
}

bool run_time_kind(std::uint32_t kind) {
    return dep_kinds.at(kind) == "RDEPEND" || dep_kinds.at(kind) == "PDEPEND";
}

std::vector<Atom> system_atoms(const Store& store) {
    std::vector<Atom> found;
    for (const auto& root : store.roots) {
        if (store.string(root.set) == "system" && store.string(root.via).empty()) {
            if (auto atom = parse_atom(store.string(root.atom))) {
                found.push_back(std::move(*atom));
            }
        }
    }
    return found;
}

} // namespace

std::optional<MergeWaitScope> merge_wait_scope(std::string_view name) {
    constexpr std::array<std::pair<std::string_view, MergeWaitScope>, 4> names{
        {{"deep", MergeWaitScope::deep},
         {"system", MergeWaitScope::system},
         {"toolchain", MergeWaitScope::toolchain},
         {"none", MergeWaitScope::none}}};
    const auto found =
        std::ranges::find(names, name, &std::pair<std::string_view, MergeWaitScope>::first);
    return found == names.end() ? std::nullopt : std::optional{found->second};
}

std::vector<std::uint32_t> run_time_waits(const Plan& plan, std::uint32_t merge) {
    std::vector<std::uint32_t> found;
    for (const auto& wait : plan.merges.at(merge).waits) {
        if ((wait.kinds.run || wait.kinds.post) && wait.merge != merge) {
            found.push_back(wait.merge);
        }
    }
    return found;
}

std::vector<bool> merge_wait_steps(const Store& store, const Graph& graph,
                                   const Evaluated& original, const Plan& plan,
                                   std::span<const Step> steps, MergeWaitScope scope) {
    const auto& evaluated = plan.evaluated_or(original);
    std::vector<bool> alone(steps.size(), false);
    if (scope == MergeWaitScope::none) {
        return alone;
    }
    const auto atoms = system_atoms(store);
    std::vector<bool> held(plan.merges.size(), false);
    // By installed package, the merge replacing it.
    std::vector<std::optional<std::uint32_t>> replaced_by(store.packages.size());
    for (std::uint32_t merge = 0; merge < plan.merges.size(); ++merge) {
        if (const auto id = plan.merges.at(merge).replaces) {
            replaced_by.at(*id) = merge;
        }
    }
    std::vector<std::uint32_t> merges;
    for (std::uint32_t merge = 0; merge < plan.merges.size(); ++merge) {
        const auto& candidate = evaluated.candidates.at(plan.merges.at(merge).candidate);
        const bool selected =
            scope == MergeWaitScope::toolchain
                ? std::ranges::contains(core_toolchain, evaluated.string(candidate.cp))
                : std::ranges::any_of(atoms, [&](const Atom& atom) {
                      return matches(store, evaluated, candidate, atom);
                  });
        if (selected) {
            merges.push_back(merge);
        }
    }
    if (scope == MergeWaitScope::deep) {
        // Installed @system members that stay, and what they reach at run time.
        std::vector<bool> seen(store.packages.size(), false);
        std::vector<std::uint32_t> installed;
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (!replaced_by.at(id) && std::ranges::any_of(atoms, [&](const Atom& atom) {
                    return matches(store, store.packages.at(id), atom);
                })) {
                installed.push_back(id);
            }
        }
        while (!installed.empty()) {
            const auto id = installed.back();
            installed.pop_back();
            if (seen.at(id)) {
                continue;
            }
            seen.at(id) = true;
            for (const auto& edge : graph.deps(id)) {
                if (!run_time_kind(edge.kind)) {
                    continue;
                }
                if (const auto merge = replaced_by.at(edge.child)) {
                    merges.push_back(*merge);
                } else {
                    installed.push_back(edge.child);
                }
            }
        }
        // Then what the merges reach.
        while (!merges.empty()) {
            const auto merge = merges.back();
            merges.pop_back();
            if (held.at(merge)) {
                continue;
            }
            held.at(merge) = true;
            for (const auto& wait : plan.merges.at(merge).waits) {
                if (run_time(wait.kinds)) {
                    merges.push_back(wait.merge);
                }
            }
        }
    } else {
        for (const auto merge : merges) {
            held.at(merge) = true;
        }
    }
    std::size_t at = 0;
    for (const auto& step : steps) {
        if (const auto* merge = std::get_if<MergeStep>(&step)) {
            alone.at(at) = held.at(merge->merge);
        }
        ++at;
    }
    return alone;
}

} // namespace egraph
