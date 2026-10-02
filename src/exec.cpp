#include "exec.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace egraph {

namespace {

using Json = nlohmann::json;

const Node& element(std::span<const Node> nodes, std::size_t index) {
    return nodes.subspan(index, 1).front();
}

bool is_blocker(const Node& node) {
    return node.type == NodeType::weak_blocker || node.type == NodeType::strong_blocker;
}

// The kinds emerge's scheduler reads blockers from: what a merged package needs at run time.
bool runtime_kind(std::size_t kind) {
    const auto name = dep_kinds.at(kind);
    return name == "IDEPEND" || name == "PDEPEND" || name == "RDEPEND";
}

// A blocker portage's dep_check keeps: under no ||.
bool outside_any_of(std::span<const Node> list, std::size_t index) {
    for (auto parent = element(list, index).parent; parent != no_parent;
         parent = element(list, parent).parent) {
        if (element(list, parent).type == NodeType::any_of) {
            return false;
        }
    }
    return true;
}

// A blocker's atom without its "!"s; a slot operator's sub-slot is only what the package was
// built against, not a bound.
std::optional<Atom> blocker_atom(std::string_view text) {
    auto parsed = parse_atom(text.substr(std::min(text.find_first_not_of('!'), text.size())));
    if (!parsed) {
        return std::nullopt;
    }
    if (parsed->slot_operator) {
        parsed->sub_slot.reset();
    }
    return std::move(*parsed);
}

// How specific best_match_to_list ranks an atom: =cpv, ~cpv, =cpv*, a slot, a comparison, the
// cp alone.
int specificity(const Atom& atom) {
    int rank = 1;
    switch (atom.op) {
    case Operator::equal:
        rank = 6;
        break;
    case Operator::approximately:
        rank = 5;
        break;
    case Operator::glob:
        rank = 4;
        break;
    case Operator::less:
    case Operator::less_equal:
    case Operator::greater_equal:
    case Operator::greater:
        rank = 2;
        break;
    case Operator::none:
        break;
    }
    return atom.slot ? std::max(rank, 3) : rank;
}

struct Named {
    std::string text;
    Atom atom;
};

// Of the atoms texts that match candidate, the most specific, the first of equals.
std::optional<Named> best_for(const Store& store, const Evaluated& evaluated,
                              const Candidate& candidate, const std::vector<std::string>& texts) {
    std::optional<Named> best;
    for (const auto& text : texts) {
        auto atom = parse_atom(text);
        if (!atom || !matches(store, evaluated, candidate, *atom)) {
            continue;
        }
        if (!best || specificity(*atom) > specificity(best->atom)) {
            best = Named{.text = text, .atom = std::move(*atom)};
        }
    }
    return best;
}

// The atoms of a root set; of @selected, only the world file's own.
std::vector<std::string> root_atoms(const Store& store, std::string_view set) {
    std::vector<std::string> found;
    for (const auto& root : store.roots) {
        if (store.string(root.set) == set && store.string(root.via).empty()) {
            found.emplace_back(store.string(root.atom));
        }
    }
    return found;
}

std::vector<std::string> argument_atoms(std::span<const Argument> arguments) {
    std::vector<std::string> found;
    for (const auto& argument : arguments) {
        if (argument.set.empty()) {
            found.push_back(argument.atom);
        }
    }
    return found;
}

// More than one slot, or one other than 0.
bool slotted(const std::set<std::string, std::less<>>& slots) {
    return slots.size() > 1 || (slots.size() == 1 && !slots.contains("0"));
}

class Run {
  public:
    Run(const Store& store EGRAPH_KEPT_BY_THIS, const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
        const Plan& plan EGRAPH_KEPT_BY_THIS)
        : store_ref_(store), evaluated_ref_(evaluated), plan_ref_(plan),
          present_(store.packages.size(), true) {}

    // The installed packages still present that the merge blocks or that block it, but for its
    // own slot and cpv, as emerge's BlockerDB.findInstalledBlockers and its scheduler find them.
    [[nodiscard]] std::vector<std::uint32_t> blockers(std::uint32_t merge) const {
        const auto& candidate = this->candidate(merge);
        std::set<std::uint32_t> found;
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            if (!runtime_kind(kind)) {
                continue;
            }
            const auto list = evaluated().nodes_in(candidate.deps.at(kind));
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (is_blocker(element(list, i)) && outside_any_of(list, i)) {
                    const auto ids = evaluated().ids_in(element(list, i).matches);
                    found.insert(ids.begin(), ids.end());
                }
            }
        }
        const auto cp = evaluated().string(candidate.cp);
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            if (present_.at(id) && blocks(store().packages.at(id), candidate, cp)) {
                found.insert(id);
            }
        }
        std::vector<std::uint32_t> kept;
        for (const auto id : found) {
            const auto& pkg = store().packages.at(id);
            const bool own = store().string(pkg.cp) == cp &&
                             (store().string(pkg.slot) == evaluated().string(candidate.slot) ||
                              store().string(pkg.cpv) == evaluated().string(candidate.cpv));
            if (present_.at(id) && !own) {
                kept.push_back(id);
            }
        }
        return kept;
    }

    // Once merged, what it replaces in its cpv or slot is gone.
    void merged(std::uint32_t merge) {
        const auto& candidate = this->candidate(merge);
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            const auto& pkg = store().packages.at(id);
            if (store().string(pkg.cp) == evaluated().string(candidate.cp) &&
                (store().string(pkg.cpv) == evaluated().string(candidate.cpv) ||
                 store().string(pkg.slot) == evaluated().string(candidate.slot))) {
                present_.at(id) = false;
            }
        }
    }

    void uninstalled(std::uint32_t package) { present_.at(package) = false; }

  private:
    std::reference_wrapper<const Store> store_ref_;
    std::reference_wrapper<const Evaluated> evaluated_ref_;
    std::reference_wrapper<const Plan> plan_ref_;
    // Per installed package, whether emerge's scheduler still counts it installed.
    std::vector<bool> present_;

    [[nodiscard]] const Store& store() const { return store_ref_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_ref_.get(); }

    [[nodiscard]] const Candidate& candidate(std::uint32_t merge) const {
        return evaluated().candidates.at(plan_ref_.get().merges.at(merge).candidate);
    }

    // One of pkg's run-time blockers matches candidate.
    [[nodiscard]] bool blocks(const Package& pkg, const Candidate& candidate,
                              std::string_view cp) const {
        for (std::size_t kind = 0; kind < dep_kinds.size(); ++kind) {
            if (!runtime_kind(kind)) {
                continue;
            }
            const auto list = store().nodes_in(pkg.deps.at(kind));
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (!is_blocker(element(list, i)) || !outside_any_of(list, i)) {
                    continue;
                }
                const auto atom = blocker_atom(store().string(element(list, i).atom));
                if (atom && atom->cp == cp && matches(store(), evaluated(), candidate, *atom)) {
                    return true;
                }
            }
        }
        return false;
    }
};

} // namespace

std::optional<std::string> world_atom(const Store& store, const Evaluated& evaluated,
                                      const Candidate& candidate,
                                      std::span<const Argument> arguments) {
    const auto argument = best_for(store, evaluated, candidate, argument_atoms(arguments));
    if (!argument) {
        return std::nullopt;
    }
    const std::string cp{evaluated.string(candidate.cp)};
    const auto& repo = argument->atom.repo;
    const auto with_repo = [&repo](const std::string& atom) {
        return repo ? atom + "::" + *repo : atom;
    };
    const auto in_repos = [&](const Candidate& other) {
        return !repo || evaluated.string(other.repo) == *repo;
    };
    const auto of_cp = [&](const Candidate& other) {
        return other.visible() && evaluated.string(other.cp) == cp;
    };
    std::set<std::string, std::less<>> slots;
    for (const auto& other : evaluated.candidates) {
        if (of_cp(other) && in_repos(other)) {
            slots.emplace(evaluated.string(other.slot));
        }
    }
    bool is_slotted = slotted(slots);
    if (!is_slotted) {
        slots.clear();
        for (const auto& pkg : store.packages) {
            if (store.string(pkg.cp) == cp) {
                slots.emplace(store.string(pkg.slot));
            }
        }
        is_slotted = slotted(slots);
    }
    auto world = with_repo(cp);
    const auto text = std::string_view{argument->text};
    if (is_slotted && text.substr(0, text.find("::")) != cp) {
        const std::string slot{evaluated.string(candidate.slot)};
        // The slot from the repositories, else from the installed packages.
        const bool available = std::ranges::any_of(evaluated.candidates, [&](const auto& other) {
            return of_cp(other) && evaluated.string(other.slot) == slot;
        });
        std::set<std::string, std::less<>> matched{slot};
        if (available) {
            for (const auto& other : evaluated.candidates) {
                if (of_cp(other) && in_repos(other) &&
                    matches(store, evaluated, other, argument->atom)) {
                    matched.emplace(evaluated.string(other.slot));
                }
            }
        } else {
            for (const auto& pkg : store.packages) {
                if (store.string(pkg.cp) == cp && matches(store, pkg, argument->atom)) {
                    matched.emplace(store.string(pkg.slot));
                }
            }
        }
        if (matched.size() == 1) {
            world = with_repo(cp + ":" + slot);
        }
    }
    if (const auto selected = best_for(store, evaluated, candidate, root_atoms(store, "selected"));
        selected && selected->text == world) {
        return std::nullopt;
    }
    // A user might want several slots of a system package selected, never one of an unslotted.
    if (!is_slotted && !repo) {
        if (const auto system = best_for(store, evaluated, candidate, root_atoms(store, "system"));
            system && !system->atom.cp.starts_with("virtual/")) {
            return std::nullopt;
        }
    }
    return world;
}

std::vector<Step> run_steps(const Store& store, const Evaluated& original, const Plan& plan,
                            std::span<const Argument> arguments, bool oneshot) {
    const auto& evaluated = plan.evaluated_or(original);
    const auto atoms = argument_atoms(arguments);
    Run run{store, evaluated, plan};
    std::vector<bool> merged(plan.merges.size(), false);
    std::vector<bool> uninstalled(plan.uninstalls.size(), false);
    std::vector<Step> steps;
    for (const auto index : plan.order) {
        const auto& candidate = evaluated.candidates.at(plan.merges.at(index).candidate);
        steps.emplace_back(MergeStep{
            .merge = index,
            .blockers = run.blockers(index),
            .world = oneshot ? std::nullopt : world_atom(store, evaluated, candidate, arguments)});
        run.merged(index);
        merged.at(index) = true;
        for (std::uint32_t u = 0; u < plan.uninstalls.size(); ++u) {
            const auto& uninstall = plan.uninstalls.at(u);
            if (uninstalled.at(u) ||
                !std::ranges::all_of(uninstall.after, [&](auto m) { return merged.at(m); })) {
                continue;
            }
            const auto& pkg = store.packages.at(uninstall.package);
            const bool argument = std::ranges::any_of(atoms, [&](const std::string& text) {
                const auto atom = parse_atom(text);
                return atom && matches(store, pkg, *atom);
            });
            steps.emplace_back(UninstallStep{.uninstall = u, .clean_world = !oneshot && argument});
            run.uninstalled(uninstall.package);
            uninstalled.at(u) = true;
        }
    }
    return steps;
}

std::string worker_request(const Store& store, const Evaluated& original, const Plan& plan,
                           const Step& step) {
    const auto& evaluated = plan.evaluated_or(original);
    if (const auto* merge = std::get_if<MergeStep>(&step)) {
        const auto& candidate = evaluated.candidates.at(plan.merges.at(merge->merge).candidate);
        Json request{{"cpv", evaluated.string(candidate.cpv)},
                     {"repo", evaluated.string(candidate.repo)}};
        if (!merge->blockers.empty()) {
            auto blockers = Json::array();
            for (const auto id : merge->blockers) {
                blockers.push_back(store.string(store.packages.at(id).cpv));
            }
            request.emplace("blockers", std::move(blockers));
        }
        if (merge->world) {
            request.emplace("world", *merge->world);
        }
        return request.dump();
    }
    const auto& uninstall = std::get<UninstallStep>(step);
    Json request{
        {"uninstall",
         store.string(store.packages.at(plan.uninstalls.at(uninstall.uninstall).package).cpv)}};
    if (uninstall.clean_world) {
        request.emplace("clean_world", true);
    }
    return request.dump();
}

} // namespace egraph
