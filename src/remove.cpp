#include "remove.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace egraph {

namespace {

// Package ids in cpv order.
void by_cpv(const Store& store, std::vector<std::uint32_t>& ids) {
    std::ranges::sort(
        ids, {}, [&store](std::uint32_t id) { return store.string(store.packages.at(id).cpv); });
}

} // namespace

Removal plan_removal(const Store& store, KeepOptions options,
                     std::span<const std::uint32_t> matched) {
    std::vector<bool> is_matched(store.packages.size(), false);
    for (const auto id : matched) {
        is_matched.at(id) = true;
    }
    options.protect.assign(store.packages.size(), false);
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        options.protect.at(id) = !is_matched.at(id);
    }
    options.without_selected = true;
    const auto kept = keep(store, options);

    Removal removal;
    std::vector<std::uint32_t> ids(matched.begin(), matched.end());
    by_cpv(store, ids);
    const auto duplicates = std::ranges::unique(ids);
    ids.erase(duplicates.begin(), duplicates.end());
    for (const auto id : ids) {
        if (!options.removed.empty() && options.removed.at(id)) {
            continue;
        }
        if (!kept.packages.at(id)) {
            removal.removed.push_back(id);
            continue;
        }
        Removal::Held held{.package = id, .dependents = {}, .sets = {}};
        for (const auto& edge : kept.pulls) {
            if (edge.child == id && edge.parent != id &&
                !std::ranges::contains(held.dependents, edge.parent)) {
                held.dependents.push_back(edge.parent);
            }
        }
        by_cpv(store, held.dependents);
        std::set<std::string> sets;
        for (const auto& pull : kept.roots) {
            if (pull.child == id) {
                // A set world_sets names, rather than the @selected it is part of.
                const auto& root = store.roots.at(pull.root);
                const auto via = store.string(root.via);
                sets.insert(std::format("@{}", via.empty() ? store.string(root.set) : via));
            }
        }
        held.sets.assign(sets.begin(), sets.end());
        removal.kept.push_back(std::move(held));
    }
    return removal;
}

std::vector<std::string> removal_lines(const Store& store, const Removal& removal) {
    std::vector<std::string> lines;
    lines.reserve(removal.removed.size() + removal.kept.size());
    for (const auto id : removal.removed) {
        lines.push_back(std::format("{}\tremove", store.string(store.packages.at(id).cpv)));
    }
    for (const auto& held : removal.kept) {
        auto line = std::format("{}\tkept", store.string(store.packages.at(held.package).cpv));
        for (const auto& set : held.sets) {
            line += std::format("\t{}", set);
        }
        for (const auto dependent : held.dependents) {
            line += std::format("\t{}", store.string(store.packages.at(dependent).cpv));
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> parse_depclean(std::string_view output) {
    constexpr std::string_view marker = "All selected packages:";
    std::vector<std::string> cpvs;
    const auto at = output.find(marker);
    if (at == std::string_view::npos) {
        return cpvs;
    }
    auto line = output.substr(at + marker.size());
    line = line.substr(0, line.find('\n'));
    while (!line.empty()) {
        const auto start = line.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            break;
        }
        line.remove_prefix(start);
        const auto word = line.substr(0, line.find(' '));
        line.remove_prefix(word.size());
        cpvs.emplace_back(word.starts_with('=') ? word.substr(1) : word);
    }
    std::ranges::sort(cpvs);
    return cpvs;
}

std::vector<std::string> removal_differences(const Store& store, const Removal& removal,
                                             std::span<const std::string> emerge) {
    std::set<std::string, std::less<>> ours;
    for (const auto id : removal.removed) {
        ours.emplace(store.string(store.packages.at(id).cpv));
    }
    const std::set<std::string, std::less<>> theirs(emerge.begin(), emerge.end());
    std::vector<std::pair<std::string, std::string_view>> found;
    for (const auto& cpv : ours) {
        if (!theirs.contains(cpv)) {
            found.emplace_back(cpv, "egraph");
        }
    }
    for (const auto& cpv : theirs) {
        if (!ours.contains(cpv)) {
            found.emplace_back(cpv, "emerge");
        }
    }
    std::ranges::sort(found);
    std::vector<std::string> lines;
    lines.reserve(found.size());
    for (const auto& [cpv, side] : found) {
        lines.push_back(std::format("{}\t{}\tremove", cpv, side));
    }
    return lines;
}

std::vector<std::string> depclean_options(bool build_deps, bool dynamic) {
    std::vector<std::string> options;
    if (!build_deps) {
        options.emplace_back("--with-bdeps=n");
    }
    if (!dynamic) {
        options.emplace_back("--dynamic-deps=n");
    }
    return options;
}

} // namespace egraph
