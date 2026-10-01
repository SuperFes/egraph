#include "selection.hpp"

#include <algorithm>
#include <format>

namespace egraph {

std::set<std::string, std::less<>> world_atoms(const Store& store) {
    std::set<std::string, std::less<>> atoms;
    for (const auto& root : store.roots) {
        if (store.string(root.set) == "selected" && store.string(root.via).empty()) {
            atoms.emplace(store.string(root.atom));
        }
    }
    return atoms;
}

std::vector<std::string> selection_changes(const std::set<std::string, std::less<>>& before,
                                           const std::set<std::string, std::less<>>& after) {
    std::vector<std::pair<std::string_view, std::string_view>> found;
    for (const auto& atom : after) {
        if (!before.contains(atom)) {
            found.emplace_back(atom, "selected");
        }
    }
    for (const auto& atom : before) {
        if (!after.contains(atom)) {
            found.emplace_back(atom, "deselected");
        }
    }
    std::ranges::sort(found);
    std::vector<std::string> lines;
    lines.reserve(found.size());
    for (const auto& [atom, change] : found) {
        lines.push_back(std::format("{}\t{}", atom, change));
    }
    return lines;
}

std::vector<std::string> parse_deselect(std::string_view output) {
    constexpr std::string_view lead = ">>> Would remove ";
    std::vector<std::string> atoms;
    while (!output.empty()) {
        const auto end = output.find('\n');
        const auto line = output.substr(0, end);
        output.remove_prefix(end == std::string_view::npos ? output.size() : end + 1);
        if (!line.starts_with(lead)) {
            continue;
        }
        const auto rest = line.substr(lead.size());
        if (const auto from = rest.find(" from \""); from != std::string_view::npos) {
            atoms.emplace_back(rest.substr(0, from));
        }
    }
    std::ranges::sort(atoms);
    return atoms;
}

std::vector<std::string> deselect_lines(const Store& store, const KeepOptions& options,
                                        std::span<const std::string> atoms) {
    auto after = options;
    after.dropped.assign(store.roots.size(), false);
    for (std::uint32_t id = 0; id < store.roots.size(); ++id) {
        const auto& root = store.roots.at(id);
        if (store.string(root.set) != "selected") {
            continue;
        }
        const auto via = store.string(root.via);
        // A world_sets set goes with every atom it gives; a world file atom only as itself.
        after.dropped.at(id) = std::ranges::any_of(atoms, [&](const std::string& atom) {
            return atom.starts_with('@') ? std::string_view{atom}.substr(1) == via
                                         : via.empty() && atom == store.string(root.atom);
        });
    }
    const auto kept_before = keep(store, options);
    const auto kept_after = keep(store, after);

    std::vector<std::string> lines;
    lines.reserve(atoms.size());
    for (const auto& atom : atoms) {
        lines.push_back(std::format("{}\tdeselect", atom));
    }
    std::vector<std::string_view> orphaned;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        if (kept_before.packages.at(id) && !kept_after.packages.at(id)) {
            orphaned.push_back(store.string(store.packages.at(id).cpv));
        }
    }
    std::ranges::sort(orphaned);
    for (const auto cpv : orphaned) {
        lines.push_back(std::format("{}\torphan", cpv));
    }
    return lines;
}

} // namespace egraph
