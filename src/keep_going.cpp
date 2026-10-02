#include "keep_going.hpp"

#include "atom.hpp"
#include "graph.hpp"
#include "version.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <variant>

namespace egraph {

namespace {

template <class T> const T& element(std::span<const T> items, std::size_t index) {
    return items.subspan(index).front();
}

// A package of emerge's resume depgraph: a merge left, an installed package, or one a step done
// merged.
struct Ref {
    enum class Kind : std::uint8_t { merge, package, merged };
    Kind kind = Kind::merge;
    // A merge index, or an installed package id.
    std::uint32_t index = 0;
    auto operator<=>(const Ref&) const = default;
};

// A dependency nothing satisfies: its atom, or a || group's.
struct Unmet {
    Ref parent;
    std::vector<std::string> atoms;
};

class ResumeGraph {
  public:
    ResumeGraph(const Store& store, const Evaluated& evaluated, const Plan& plan,
                const InstalledBlockers& installed, std::vector<std::uint32_t> merged)
        : store_{store}, evaluated_{evaluated}, plan_{plan}, installed_{installed},
          merged_{std::move(merged)} {}

    // The packages, from the merges left (by merge index), that a pass finds with a dependency
    // unsatisfied, and what reaches each through the atom it was reached by.
    void pass(const std::vector<bool>& left) {
        left_ = left;
        unsatisfied_.clear();
        reached_by_.clear();
        std::set<Ref> seen;
        std::deque<Ref> queue;
        for (std::uint32_t merge = 0; merge < left.size(); ++merge) {
            if (left.at(merge)) {
                queue.push_back({.kind = Ref::Kind::merge, .index = merge});
                seen.insert(queue.back());
            }
        }
        while (!queue.empty()) {
            const auto ref = queue.front();
            queue.pop_front();
            for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
                if (ref.kind != Ref::Kind::merge &&
                    is_build_kind(static_cast<std::uint32_t>(kind))) {
                    continue;
                }
                if (ref.kind == Ref::Kind::package) {
                    const auto& pkg = store_.get().packages.at(ref.index);
                    walk(ref, store_.get(), store_.get().nodes_in(pkg.deps.at(kind)), seen, queue);
                } else {
                    walk(ref, evaluated_.get(),
                         evaluated_.get().nodes_in(candidate(ref.index).deps.at(kind)), seen,
                         queue);
                }
            }
        }
    }

    [[nodiscard]] const std::vector<Unmet>& unsatisfied() const { return unsatisfied_; }

    [[nodiscard]] std::span<const std::pair<Ref, std::string>> reached_by(const Ref& ref) const {
        const auto found = reached_by_.find(ref);
        if (found == reached_by_.end()) {
            return {};
        }
        return found->second;
    }

    // Whether something installed now matches the atom, whatever the merges left replace.
    [[nodiscard]] bool installed_match(std::string_view text) {
        const auto& atom = parsed(text);
        if (!atom) {
            return false;
        }
        const auto& store = store_.get();
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (installed_.get().present(id) &&
                store.string(store.packages.at(id).cp) == atom->cp &&
                matches(store, store.packages.at(id), *atom)) {
                return true;
            }
        }
        return std::ranges::any_of(merged_, [&](std::uint32_t merge) {
            return evaluated_.get().string(candidate(merge).cp) == atom->cp &&
                   matches(store, evaluated_.get(), candidate(merge), *atom);
        });
    }

    [[nodiscard]] std::string cpv(const Ref& ref) const {
        if (ref.kind == Ref::Kind::package) {
            return std::string(store_.get().string(store_.get().packages.at(ref.index).cpv));
        }
        return std::string(evaluated_.get().string(candidate(ref.index).cpv));
    }

  private:
    [[nodiscard]] const Candidate& candidate(std::uint32_t merge) const {
        return evaluated_.get().candidates.at(plan_.get().merges.at(merge).candidate);
    }

    // The atom, or none for one that is no atom alone (a USE dependency still conditional).
    const std::optional<Atom>& parsed(std::string_view text) {
        auto found = atoms_.find(text);
        if (found == atoms_.end()) {
            auto atom = parse_atom(text);
            found = atoms_
                        .emplace(std::string(text),
                                 atom ? std::optional{std::move(*atom)} : std::nullopt)
                        .first;
        }
        return found->second;
    }

    // Whether a merge left replaces the installed package of cp, slot and cpv.
    [[nodiscard]] bool replaced(std::string_view cp, std::string_view slot,
                                std::string_view cpv) const {
        const auto& evaluated = evaluated_.get();
        for (std::uint32_t merge = 0; merge < left_.size(); ++merge) {
            if (!left_.at(merge)) {
                continue;
            }
            const auto& other = candidate(merge);
            if (evaluated.string(other.cp) == cp &&
                (evaluated.string(other.slot) == slot || evaluated.string(other.cpv) == cpv)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<Version> version(const Ref& ref) const {
        const auto text = cpv(ref);
        const auto cp = ref.kind == Ref::Kind::package
                            ? store_.get().string(store_.get().packages.at(ref.index).cp)
                            : evaluated_.get().string(candidate(ref.index).cp);
        return parse_version(std::string_view{text}.substr(std::min(cp.size() + 1, text.size())));
    }

    // What emerge's _select_pkg_from_graph picks for the atom: the best merge left it matches,
    // else the best installed package no merge left replaces.
    [[nodiscard]] std::optional<Ref> select(const Atom& atom) const {
        const auto& store = store_.get();
        const auto& evaluated = evaluated_.get();
        std::vector<Ref> found;
        for (std::uint32_t merge = 0; merge < left_.size(); ++merge) {
            if (left_.at(merge) && evaluated.string(candidate(merge).cp) == atom.cp &&
                matches(store, evaluated, candidate(merge), atom)) {
                found.push_back({.kind = Ref::Kind::merge, .index = merge});
            }
        }
        if (found.empty()) {
            for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
                const auto& pkg = store.packages.at(id);
                if (installed_.get().present(id) && store.string(pkg.cp) == atom.cp &&
                    !replaced(store.string(pkg.cp), store.string(pkg.slot),
                              store.string(pkg.cpv)) &&
                    matches(store, pkg, atom)) {
                    found.push_back({.kind = Ref::Kind::package, .index = id});
                }
            }
            for (const auto merge : merged_) {
                const auto& done = candidate(merge);
                if (evaluated.string(done.cp) == atom.cp &&
                    !replaced(evaluated.string(done.cp), evaluated.string(done.slot),
                              evaluated.string(done.cpv)) &&
                    matches(store, evaluated, done, atom)) {
                    found.push_back({.kind = Ref::Kind::merged, .index = merge});
                }
            }
        }
        if (found.empty()) {
            return std::nullopt;
        }
        return *std::ranges::max_element(found, [this](const Ref& a, const Ref& b) {
            const auto first = version(a);
            const auto second = version(b);
            return first && second && vercmp(*first, *second) < 0;
        });
    }

    // Follows one of parent's dependency lists: each atom to what it selects, the first member
    // of a || that is satisfied, and what is satisfied by nothing recorded.
    void walk(const Ref& parent, const Tables& tables, std::span<const Node> nodes,
              std::set<Ref>& seen, std::deque<Ref>& queue) {
        const auto text = [&](std::size_t i) { return tables.string(element(nodes, i).atom); };
        const auto ok = satisfied(nodes, [&](std::size_t i) {
            const auto& atom = parsed(text(i));
            return !atom || select(*atom).has_value();
        });
        std::vector<std::vector<std::size_t>> children(nodes.size());
        std::vector<std::size_t> roots;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            (element(nodes, i).parent == no_parent ? roots : children.at(element(nodes, i).parent))
                .push_back(i);
        }
        const auto leaves = [&](std::size_t group) {
            std::vector<std::string> found;
            std::vector<std::size_t> stack{group};
            while (!stack.empty()) {
                const auto i = stack.back();
                stack.pop_back();
                if (element(nodes, i).type == NodeType::atom && !ok.at(i)) {
                    found.emplace_back(text(i));
                }
                stack.insert(stack.end(), children.at(i).rbegin(), children.at(i).rend());
            }
            return found;
        };
        const std::function<void(std::size_t)> visit = [&](std::size_t i) {
            switch (element(nodes, i).type) {
            case NodeType::atom:
                if (!ok.at(i)) {
                    unsatisfied_.push_back({.parent = parent, .atoms = {std::string(text(i))}});
                } else if (const auto& atom = parsed(text(i))) {
                    const auto child = *select(*atom);
                    reached_by_[child].emplace_back(parent, std::string(text(i)));
                    if (seen.insert(child).second) {
                        queue.push_back(child);
                    }
                }
                break;
            case NodeType::all_of:
                std::ranges::for_each(children.at(i), visit);
                break;
            case NodeType::any_of:
                if (!ok.at(i)) {
                    unsatisfied_.push_back({.parent = parent, .atoms = leaves(i)});
                } else if (const auto first = std::ranges::find_if(
                               children.at(i), [&](std::size_t child) { return ok.at(child); });
                           first != children.at(i).end()) {
                    visit(*first);
                }
                break;
            case NodeType::weak_blocker:
            case NodeType::strong_blocker:
                break;
            }
        };
        std::ranges::for_each(roots, visit);
    }

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::reference_wrapper<const Plan> plan_;
    std::reference_wrapper<const InstalledBlockers> installed_;
    // Merge indices of the steps done.
    std::vector<std::uint32_t> merged_;
    // By merge index, whether it is left.
    std::vector<bool> left_;
    std::map<std::string, std::optional<Atom>, std::less<>> atoms_;
    std::vector<Unmet> unsatisfied_;
    std::map<Ref, std::vector<std::pair<Ref, std::string>>> reached_by_;
};

} // namespace

Resumption keep_going(const Store& store, const Evaluated& evaluated, const Plan& plan,
                      std::span<const Step> steps, std::span<const Standing> standing) {
    InstalledBlockers installed{store, evaluated, plan};
    std::vector<bool> left(plan.merges.size(), false);
    std::vector<bool> done(plan.merges.size(), false);
    std::vector<std::uint32_t> merged;
    for (std::size_t step = 0; step < steps.size(); ++step) {
        const auto* merge = std::get_if<MergeStep>(&element(steps, step));
        if (element(standing, step) == Standing::done) {
            installed.done(element(steps, step));
            if (merge != nullptr) {
                done.at(merge->merge) = true;
                merged.push_back(merge->merge);
            }
        } else if (element(standing, step) == Standing::left && merge != nullptr) {
            left.at(merge->merge) = true;
        }
    }
    ResumeGraph graph{store, evaluated, plan, installed, std::move(merged)};
    std::map<std::uint32_t, std::vector<std::string>> dropped;
    for (;;) {
        graph.pass(left);
        const auto& unsatisfied = graph.unsatisfied();
        if (unsatisfied.empty()) {
            break;
        }
        // What has a dependency unsatisfied goes, and what reaches it, unless something
        // installed matches the atom, as emerge's _resume_depgraph drops them.
        std::map<Ref, std::vector<std::string>> going;
        std::set<Ref> traversed;
        std::deque<Unmet> queue(unsatisfied.begin(), unsatisfied.end());
        while (!queue.empty()) {
            auto [parent, atoms] = std::move(queue.front());
            queue.pop_front();
            if (std::ranges::any_of(
                    atoms, [&](const std::string& atom) { return graph.installed_match(atom); })) {
                continue;
            }
            auto& found = going[parent];
            for (auto& atom : atoms) {
                if (std::ranges::find(found, atom) == found.end()) {
                    found.push_back(std::move(atom));
                }
            }
            if (!traversed.insert(parent).second) {
                continue;
            }
            for (const auto& [other, atom] : graph.reached_by(parent)) {
                queue.push_back({.parent = other, .atoms = {atom}});
            }
        }
        bool shrank = false;
        for (auto& [ref, atoms] : going) {
            if (ref.kind == Ref::Kind::merge) {
                left.at(ref.index) = false;
                dropped.emplace(ref.index, std::move(atoms));
                shrank = true;
            }
        }
        if (!shrank) {
            const auto& first = unsatisfied.front();
            return {.skipped = {},
                    .stuck = std::format("{}: {}", graph.cpv(first.parent),
                                         first.atoms.empty() ? "" : first.atoms.front())};
        }
    }
    Resumption resumption;
    for (std::size_t step = 0; step < steps.size(); ++step) {
        if (element(standing, step) != Standing::left) {
            continue;
        }
        if (const auto* merge = std::get_if<MergeStep>(&element(steps, step))) {
            if (const auto found = dropped.find(merge->merge); found != dropped.end()) {
                resumption.skipped.push_back({.step = step, .atoms = found->second});
            }
        } else {
            const auto& after =
                plan.uninstalls.at(std::get<UninstallStep>(element(steps, step)).uninstall).after;
            if (std::ranges::none_of(after, [&](std::uint32_t needed) {
                    return done.at(needed) || left.at(needed);
                })) {
                resumption.skipped.push_back({.step = step, .atoms = {}});
            }
        }
    }
    return resumption;
}

std::string describe_skip(const Skip& skip) {
    if (skip.atoms.empty()) {
        return "no merge left needs it gone";
    }
    std::string text = "needs ";
    for (std::size_t i = 0; i < skip.atoms.size(); ++i) {
        text += (i == 0 ? "" : ", ") + skip.atoms.at(i);
    }
    return text;
}

} // namespace egraph
