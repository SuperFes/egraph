#include "remedy.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string_view>

namespace egraph {

bool leaf(const Store& store, const Holder& holder) {
    return holder.dependents.empty() && std::ranges::all_of(holder.roots, [&](std::uint32_t root) {
               return store.string(store.roots.at(root).set) == "selected";
           });
}

std::vector<Remedy> remedies(const Store& store, const Evaluated& evaluated, const Graph& graph,
                             const Plan& plan, UseRebuilds rebuilds, const Targets& targets,
                             const Rescope& rescope) {
    std::vector<std::vector<std::uint32_t>> roots_of(store.packages.size());
    for (std::uint32_t root = 0; root < store.roots.size(); ++root) {
        for (const auto id : store.ids_in(store.roots.at(root).matches)) {
            roots_of.at(id).push_back(root);
        }
    }
    // A candidate of an installed package's own version stands for that package: its rebuild.
    std::map<std::string_view, std::uint32_t, std::less<>> by_cpv;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        by_cpv.emplace(store.string(store.packages.at(id).cpv), id);
    }
    std::vector<Remedy> found;
    for (std::uint32_t index = 0; index < plan.held.size(); ++index) {
        const auto& back = plan.held.at(index);
        Remedy remedy{
            .held = index, .holders = {}, .nodeps = false, .removable = false, .frees = {}};
        std::vector<std::uint32_t> ids;
        bool own = false;
        for (const auto& reason : back.reasons) {
            if (!reason.member.candidate) {
                ids.push_back(reason.member.index);
                continue;
            }
            const auto installed =
                by_cpv.find(evaluated.string(evaluated.candidates.at(reason.member.index).cpv));
            if (installed != by_cpv.end() && installed->second != back.package) {
                ids.push_back(installed->second);
            } else {
                own = true;
            }
        }
        std::ranges::sort(ids);
        const auto [first, last] = std::ranges::unique(ids);
        ids.erase(first, last);
        for (const auto id : ids) {
            Holder holder{.package = id, .dependents = {}, .roots = roots_of.at(id)};
            for (const auto& edge : graph.rdeps(id)) {
                if (edge.parent != id && !std::ranges::binary_search(ids, edge.parent)) {
                    holder.dependents.push_back(edge.parent);
                }
            }
            std::ranges::sort(holder.dependents);
            const auto [from, to] = std::ranges::unique(holder.dependents);
            holder.dependents.erase(from, to);
            remedy.holders.push_back(std::move(holder));
        }
        remedy.nodeps = !own && !ids.empty();
        if (remedy.nodeps && std::ranges::all_of(remedy.holders, [&](const Holder& holder) {
                return leaf(store, holder);
            })) {
            std::vector<bool> removed(store.packages.size());
            for (const auto id : ids) {
                removed.at(id) = true;
            }
            auto scope = rescope ? rescope(removed) : targets.scope;
            if (scope.empty()) {
                scope.assign(store.packages.size(), true);
            }
            for (const auto id : ids) {
                scope.at(id) = false;
            }
            // Its own dependencies weigh even where nothing keeps it any more.
            scope.at(back.package) = true;
            const auto after =
                plan_updates(store, evaluated, rebuilds, {.scope = scope, .roots = targets.roots});
            std::set<std::uint32_t> still;
            for (const auto& held : after.held) {
                still.insert(held.package);
            }
            if (!still.contains(back.package)) {
                remedy.removable = true;
                for (std::uint32_t other = 0; other < plan.held.size(); ++other) {
                    const auto package = plan.held.at(other).package;
                    if (other != index && scope.at(package) && !still.contains(package)) {
                        remedy.frees.push_back(other);
                    }
                }
            }
        }
        found.push_back(std::move(remedy));
    }
    return found;
}

} // namespace egraph
