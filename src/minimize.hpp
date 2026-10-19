#pragma once

// emerge's _minimize_children, shared by depclean and the plan.

#include <algorithm>
#include <functional>
#include <span>
#include <vector>

namespace egraph {

// Of the packages one dependency list's atoms select within a cp, in order (installed first,
// each lowest first), drops each while every atom matching it matches another one left. Each of
// lists, through project, holds the selected packages one atom matches; dropped ones are erased
// from all of them.
template <typename Pkg, typename Lists, typename Projection = std::identity>
void minimize_children(std::span<const Pkg> order, Lists& lists, Projection project = {}) {
    for (const auto& pkg : order) {
        const auto needed = std::ranges::any_of(lists, [&](auto& entry) {
            const std::vector<Pkg>& matched = std::invoke(project, entry);
            return matched.size() < 2 && std::ranges::contains(matched, pkg);
        });
        if (!needed) {
            for (auto& entry : lists) {
                std::erase(std::invoke(project, entry), pkg);
            }
        }
    }
}

} // namespace egraph
