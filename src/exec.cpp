#include "exec.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
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

} // namespace

InstalledBlockers::InstalledBlockers(const Store& store, const Evaluated& evaluated,
                                     const Plan& plan)
    : store_ref_(store), evaluated_ref_(evaluated), plan_ref_(plan),
      present_(store.packages.size(), true) {}

std::vector<std::uint32_t> InstalledBlockers::blockers(std::uint32_t merge) const {
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

void InstalledBlockers::merged(std::uint32_t merge) {
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

void InstalledBlockers::uninstalled(std::uint32_t package) {
    present_.at(package) = false;
}

void InstalledBlockers::done(const Step& step) {
    if (const auto* merge = std::get_if<MergeStep>(&step)) {
        merged(merge->merge);
    } else {
        uninstalled(plan_ref_.get().uninstalls.at(std::get<UninstallStep>(step).uninstall).package);
    }
}

const Candidate& InstalledBlockers::candidate(std::uint32_t merge) const {
    return evaluated().candidates.at(plan_ref_.get().merges.at(merge).candidate);
}

bool InstalledBlockers::blocks(const Package& pkg, const Candidate& candidate,
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
    InstalledBlockers run{store, evaluated, plan};
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

namespace {

// A merge's blockers and world atom among its request's fields.
void add_merge_fields(Json& request, const Store& store, std::span<const std::uint32_t> blockers,
                      const std::optional<std::string>& world) {
    if (!blockers.empty()) {
        auto cpvs = Json::array();
        for (const auto id : blockers) {
            cpvs.push_back(store.string(store.packages.at(id).cpv));
        }
        request.emplace("blockers", std::move(cpvs));
    }
    if (world) {
        request.emplace("world", *world);
    }
}

std::string uninstall_request(const Store& store, const Plan& plan, const UninstallStep& step) {
    Json request{{"uninstall",
                  store.string(store.packages.at(plan.uninstalls.at(step.uninstall).package).cpv)}};
    if (step.clean_world) {
        request.emplace("clean_world", true);
    }
    return request.dump();
}

const Candidate& merged_candidate(const Evaluated& evaluated, const Plan& plan,
                                  const MergeStep& step) {
    return evaluated.candidates.at(plan.merges.at(step.merge).candidate);
}

} // namespace

std::string worker_request(const Store& store, const Evaluated& original, const Plan& plan,
                           const Step& step) {
    const auto& evaluated = plan.evaluated_or(original);
    if (const auto* merge = std::get_if<MergeStep>(&step)) {
        const auto& candidate = merged_candidate(evaluated, plan, *merge);
        Json request{{"cpv", evaluated.string(candidate.cpv)},
                     {"repo", evaluated.string(candidate.repo)}};
        add_merge_fields(request, store, merge->blockers, merge->world);
        return request.dump();
    }
    return uninstall_request(store, plan, std::get<UninstallStep>(step));
}

StepRequests::StepRequests(const Store& store, const Evaluated& evaluated, const Plan& plan,
                           std::vector<Step> steps)
    : store_ref_(store), evaluated_ref_(plan.evaluated_or(evaluated)), plan_ref_(plan),
      steps_(std::move(steps)), blockers_(store, plan.evaluated_or(evaluated), plan) {}

std::string StepRequests::build(std::size_t step) const {
    const auto& evaluated = evaluated_ref_.get();
    const auto& candidate =
        merged_candidate(evaluated, plan_ref_.get(), std::get<MergeStep>(steps_.at(step)));
    return Json{{"build", evaluated.string(candidate.cpv)},
                {"repo", evaluated.string(candidate.repo)}}
        .dump();
}

std::string StepRequests::merge(std::size_t step) const {
    const auto* merge = std::get_if<MergeStep>(&steps_.at(step));
    if (merge == nullptr) {
        return uninstall_request(store_ref_.get(), plan_ref_.get(),
                                 std::get<UninstallStep>(steps_.at(step)));
    }
    const auto& evaluated = evaluated_ref_.get();
    Json request{
        {"merge", evaluated.string(merged_candidate(evaluated, plan_ref_.get(), *merge).cpv)}};
    add_merge_fields(request, store_ref_.get(), blockers_.blockers(merge->merge), merge->world);
    return request.dump();
}

void StepRequests::done(std::size_t step) {
    blockers_.done(steps_.at(step));
}

std::string step_cpv(const Store& store, const Evaluated& original, const Plan& plan,
                     const Step& step) {
    const auto& evaluated = plan.evaluated_or(original);
    if (const auto* merge = std::get_if<MergeStep>(&step)) {
        return std::string{
            evaluated.string(evaluated.candidates.at(plan.merges.at(merge->merge).candidate).cpv)};
    }
    const auto& uninstall = plan.uninstalls.at(std::get<UninstallStep>(step).uninstall);
    return std::string{store.string(store.packages.at(uninstall.package).cpv)};
}

std::expected<WorkerEvent, std::string> parse_event(std::string_view line) {
    using Kind = WorkerEvent::Kind;
    const auto fields = Json::parse(line, nullptr, false);
    if (!fields.is_object()) {
        return std::unexpected(std::format("not a JSON object: {}", line));
    }
    for (const auto& [name, kind] : {std::pair{"phase", Kind::phase},
                                     {"built", Kind::built},
                                     {"merged", Kind::merged},
                                     {"uninstalled", Kind::uninstalled},
                                     {"failed", Kind::failed},
                                     {"error", Kind::error}}) {
        const auto found = fields.find(name);
        if (found == fields.end()) {
            continue;
        }
        if (!found->is_string()) {
            break;
        }
        WorkerEvent event{.kind = kind, .text = found->get<std::string>(), .status = 0, .log = ""};
        if (kind == Kind::failed) {
            const auto status = fields.find("status");
            const auto log = fields.find("log");
            if (status == fields.end() || !status->is_number_integer() || log == fields.end() ||
                !log->is_string()) {
                break;
            }
            event.status = status->get<int>();
            event.log = log->get<std::string>();
        }
        return event;
    }
    return std::unexpected(std::format("not an event: {}", line));
}

bool final_event(const WorkerEvent& event) {
    return event.kind != WorkerEvent::Kind::phase;
}

std::string describe_event(const WorkerEvent& event) {
    using Kind = WorkerEvent::Kind;
    switch (event.kind) {
    case Kind::built:
        return "built";
    case Kind::merged:
        return "merged";
    case Kind::uninstalled:
        return "uninstalled";
    case Kind::failed:
        return std::format("{} failed with status {} (log: {})", event.text, event.status,
                           event.log);
    case Kind::phase:
    case Kind::error:
        break;
    }
    return event.text;
}

} // namespace egraph
