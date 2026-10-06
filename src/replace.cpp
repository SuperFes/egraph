#include "replace.hpp"

#include "graph.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <system_error>

namespace egraph {

namespace {

const Node& element(std::span<const Node> nodes, std::size_t index) {
    return nodes.subspan(index, 1).front();
}

// What a plan leaves installed: the packages no merge replaces and no uninstall removes, beside
// its merges.
class Staying {
  public:
    Staying(const Store& store, const Evaluated& evaluated, const Plan& plan)
        : store_{store}, evaluated_{evaluated}, plan_{plan}, gone_(store.packages.size(), false) {
        for (const auto& merge : plan.merges) {
            if (merge.replaces) {
                gone_.at(*merge.replaces) = true;
            }
        }
        for (const auto& each : plan.uninstalls) {
            gone_.at(each.package) = true;
        }
    }

    [[nodiscard]] bool gone(std::uint32_t id) const { return gone_.at(id); }
    void remove(std::uint32_t id) { gone_.at(id) = true; }

    // Whether what stays, without package, needs it: a root atom or a list of dependencies it
    // satisfies that nothing else does.
    [[nodiscard]] bool needs(std::uint32_t package) const {
        for (const auto& root : store_.get().roots) {
            const auto installed = store_.get().ids_in(root.matches);
            if (std::ranges::contains(installed, package) &&
                !satisfied_without(package, installed, store_.get().string(root.atom))) {
                return true;
            }
        }
        for (std::uint32_t id = 0; id < store_.get().packages.size(); ++id) {
            if (id == package || gone(id)) {
                continue;
            }
            for (const auto range : store_.get().packages.at(id).deps) {
                if (needed_by(package, store_.get(), store_.get().nodes_in(range))) {
                    return true;
                }
            }
        }
        for (const auto& merge : plan_.get().merges) {
            for (const auto range : evaluated_.get().candidates.at(merge.candidate).deps) {
                if (needed_by(package, evaluated_.get(), evaluated_.get().nodes_in(range))) {
                    return true;
                }
            }
        }
        return false;
    }

  private:
    // Whether an atom matching these installed packages is satisfied once package is gone.
    [[nodiscard]] bool satisfied_without(std::uint32_t package,
                                         std::span<const std::uint32_t> installed,
                                         std::string_view text) const {
        if (std::ranges::any_of(installed, [&](auto id) { return id != package && !gone(id); })) {
            return true;
        }
        // One the store holds parse_atom cannot read counts as unsatisfied, keeping package.
        const auto atom = parse_atom(text);
        return atom && std::ranges::any_of(plan_.get().merges, [&](const Merge& merge) {
                   return matches(store_.get(), evaluated_.get(),
                                  evaluated_.get().candidates.at(merge.candidate), *atom);
               });
    }

    [[nodiscard]] bool needed_by(std::uint32_t package, const Tables& tables,
                                 std::span<const Node> nodes) const {
        if (std::ranges::none_of(nodes, [&](const Node& node) {
                return node.type == NodeType::atom &&
                       std::ranges::contains(tables.ids_in(node.matches), package);
            })) {
            return false;
        }
        const auto top_level = [&](const std::vector<bool>& result) {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (element(nodes, i).parent == no_parent && !result.at(i)) {
                    return false;
                }
            }
            return true;
        };
        const auto with = satisfied(nodes, [&](std::size_t i) {
            return std::ranges::any_of(tables.ids_in(element(nodes, i).matches),
                                       [&](auto id) { return id == package || !gone(id); }) ||
                   satisfied_without(package, {}, tables.string(element(nodes, i).atom));
        });
        const auto without = satisfied(nodes, [&](std::size_t i) {
            const auto& node = element(nodes, i);
            return satisfied_without(package, tables.ids_in(node.matches),
                                     tables.string(node.atom));
        });
        return top_level(with) && !top_level(without);
    }

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::reference_wrapper<const Plan> plan_;
    std::vector<bool> gone_;
};

} // namespace

std::expected<std::vector<Atom>, std::string> parse_replace_slots(std::string_view text) {
    std::vector<Atom> found;
    std::size_t number = 0;
    for (const auto line : std::views::split(text, '\n')) {
        ++number;
        auto entry = std::string_view{line.begin(), line.end()};
        entry = entry.substr(0, entry.find('#'));
        const auto first = entry.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) {
            continue;
        }
        entry = entry.substr(first, entry.find_last_not_of(" \t\r") + 1 - first);
        auto atom = parse_atom(entry);
        if (!atom) {
            return std::unexpected(std::format("line {}: {}", number, atom.error()));
        }
        found.push_back(std::move(*atom));
    }
    return found;
}

std::filesystem::path replace_slots_path(const std::filesystem::path& config_root) {
    return config_root / "etc/egraph/replace-slots";
}

std::expected<std::vector<Atom>, std::string>
read_replace_slots(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return std::vector<Atom>{};
    }
    std::ifstream in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    if (!in) {
        return std::unexpected(std::format("{}: cannot read it", path.string()));
    }
    auto found = parse_replace_slots(text.str());
    if (!found) {
        return std::unexpected(std::format("{}: {}", path.string(), found.error()));
    }
    return found;
}

void replace_slots(const Store& store, const Evaluated& evaluated, std::span<const Atom> atoms,
                   Plan& plan) {
    if (atoms.empty()) {
        return;
    }
    Staying staying{store, evaluated, plan};
    // Each listed package a merge new in its slot replaces, with those merges.
    std::map<std::uint32_t, std::vector<std::uint32_t>> replacing;
    for (const auto& root : store.roots) {
        const auto atom = parse_atom(store.string(root.atom));
        if (!atom || atom->slot) {
            continue;
        }
        for (std::uint32_t m = 0; m < plan.merges.size(); ++m) {
            const auto& merge = plan.merges.at(m);
            const auto& candidate = evaluated.candidates.at(merge.candidate);
            if (merge.replaces || !matches(store, evaluated, candidate, *atom)) {
                continue;
            }
            for (const auto id : store.ids_in(root.matches)) {
                const auto& pkg = store.packages.at(id);
                if (!staying.gone(id) &&
                    store.string(pkg.slot) != evaluated.string(candidate.slot) &&
                    std::ranges::any_of(
                        atoms, [&](const Atom& listed) { return matches(store, pkg, listed); })) {
                    replacing[id].push_back(m);
                }
            }
        }
    }
    for (auto& [id, merges] : replacing) {
        if (staying.needs(id)) {
            continue;
        }
        std::ranges::sort(merges);
        const auto duplicates = std::ranges::unique(merges);
        merges.erase(duplicates.begin(), duplicates.end());
        staying.remove(id);
        plan.uninstalls.push_back({.package = id, .why = std::nullopt, .after = std::move(merges)});
    }
}

} // namespace egraph
