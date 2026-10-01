#include "request.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace egraph {

namespace {

// The sets @world takes in, as portage's world set names them.
constexpr std::array<std::string_view, 3> world_sets{"selected", "system", "profile"};

bool known_set(std::string_view name) {
    return name == "world" || std::ranges::contains(world_sets, name);
}

using Cps = std::set<std::string, std::less<>>;

// The cps the stores answer for: installed, with candidates, or evaluated on request (with none
// visible).
Cps evaluated_cps(const Store& store, const Evaluated& evaluated) {
    Cps cps;
    for (const auto& pkg : store.packages) {
        cps.emplace(store.string(pkg.cp));
    }
    for (const auto& candidate : evaluated.candidates) {
        cps.emplace(evaluated.string(candidate.cp));
    }
    for (const auto id : evaluated.ids_in(evaluated.requested)) {
        cps.emplace(evaluated.string(id));
    }
    return cps;
}

// The categories emerge passes over for a name that another category has too.
bool stands_aside(std::string_view cp) {
    return cp.starts_with("virtual/") || cp.starts_with("acct-group/") ||
           cp.starts_with("acct-user/");
}

// The word with its category filled in from the one cp in cps with its name, as emerge fills it.
std::expected<std::string, std::string> with_category(std::string_view word, const Cps& cps) {
    const auto name_at = word.find_first_not_of("<>=~");
    const auto op = word.substr(0, name_at == std::string_view::npos ? word.size() : name_at);
    const auto rest = word.substr(op.size());
    if (rest.contains('/')) {
        return std::string(word);
    }
    std::vector<std::string> found;
    for (const auto& cp : cps) {
        const auto name = std::string_view{cp}.substr(cp.find('/') + 1);
        if (!rest.starts_with(name)) {
            continue;
        }
        auto text = std::format("{}{}/{}", op, cp.substr(0, cp.find('/')), rest);
        if (const auto atom = parse_atom(text); atom && atom->cp == cp) {
            found.push_back(std::move(text));
        }
    }
    if (found.empty()) {
        return std::unexpected(
            std::format("{}: no package by that name in the repositories", word));
    }
    if (found.size() > 1) {
        const auto aside = [](const std::string& text) { return stands_aside(text); };
        if (std::ranges::count_if(found, std::not_fn(aside)) == 1) {
            return *std::ranges::find_if_not(found, aside);
        }
        std::string listed;
        for (std::size_t i = 0; i < found.size(); ++i) {
            const auto cp = parse_atom(found.at(i))->cp;
            listed += std::format("{}{}", i == 0 ? "" : i + 1 == found.size() ? " and " : ", ", cp);
        }
        return std::unexpected(std::format("{}: ambiguous, in {}", word, listed));
    }
    return std::move(found.front());
}

bool matches_any(const Store& store, const Evaluated& evaluated, const Atom& atom,
                 bool visible_only) {
    if (!visible_only && std::ranges::any_of(store.packages, [&](const Package& pkg) {
            return matches(store, pkg, atom);
        })) {
        return true;
    }
    return std::ranges::any_of(evaluated.candidates, [&](const Candidate& candidate) {
        return (!visible_only || candidate.visible()) && matches(store, evaluated, candidate, atom);
    });
}

// An error unless something installed or a visible ebuild matches the atom text.
std::optional<std::string> refusal(const Store& store, const Evaluated& evaluated,
                                   std::string_view text) {
    const auto atom = parse_atom(text);
    const auto exact = parse_atom(std::format("={}", text));
    const auto hint = exact && matches_any(store, evaluated, *exact, false)
                          ? std::format("; ={} names that version", text)
                          : std::string{};
    if (!atom) {
        return hint.empty() ? std::format("{}: {}", text, atom.error())
                            : std::format("{}: nothing matches{}", text, hint);
    }
    const bool installed = std::ranges::any_of(
        store.packages, [&](const Package& pkg) { return matches(store, pkg, *atom); });
    if (installed || matches_any(store, evaluated, *atom, true)) {
        return std::nullopt;
    }
    // A USE change can meet its USE dependencies, as emerge's autounmask asks.
    auto plain = *atom;
    plain.use.clear();
    if (!atom->use.empty() && matches_any(store, evaluated, plain, true)) {
        return std::nullopt;
    }
    if (matches_any(store, evaluated, *atom, false)) {
        return std::format("{}: every ebuild that matches is masked", text);
    }
    // The builder keeps no masked ebuild of what is not installed.
    if (std::ranges::contains(evaluated.ids_in(evaluated.requested), atom->cp,
                              [&](std::uint32_t id) { return evaluated.string(id); })) {
        return std::format("{}: nothing visible matches{}", text, hint);
    }
    return std::format("{}: nothing matches{}", text, hint);
}

const Node& element(std::span<const Node> nodes, std::size_t index) {
    return nodes.subspan(index, 1).front();
}

std::optional<Version> version_of(std::string_view cpv, std::string_view cp) {
    return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
}

// emerge --deep's walk of its graph from the arguments (depgraph._create_graph): each package
// follows the version it ends up with, and its ||s wait until the plain dependencies are in,
// each then taking one alternative.
class Reach {
  public:
    Reach(const Store& store EGRAPH_KEPT_BY_THIS, const Evaluated& evaluated EGRAPH_KEPT_BY_THIS,
          const Request& request)
        : store_(store), evaluated_(evaluated), reached_(store.packages.size()),
          taken_(evaluated.candidates.size()) {
        for (const auto& argument : request.arguments) {
            if (auto atom = parse_atom(argument.atom)) {
                // What a USE change could have it merge reaches too.
                atom->use.clear();
                cps_.insert(atom->cp);
                atoms_.push_back(std::move(*atom));
            }
        }
        for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
            const auto& candidate = evaluated.candidates.at(i);
            if (candidate.visible()) {
                visible_[std::string(evaluated.string(candidate.cp))].push_back(i);
            }
        }
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            installed_[std::string(store.string(store.packages.at(id).cp))].push_back(id);
        }
    }

    std::vector<bool> walk() {
        for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
            if (named(id)) {
                reach(id);
            }
        }
        for (const auto& atom : atoms_) {
            if (const auto best = best_match(atom)) {
                take(*best);
            }
        }
        while (!stack_.empty() || !disjunctions_.empty()) {
            while (!stack_.empty()) {
                const auto step = stack_.back();
                stack_.pop_back();
                follow(step);
            }
            if (!disjunctions_.empty()) {
                const auto step = disjunctions_.back();
                disjunctions_.pop_back();
                choose(step);
            }
        }
        return std::move(reached_);
    }

  private:
    // An installed package's dependencies, or a candidate's.
    struct Step {
        bool candidate = false;
        std::uint32_t index = 0;
    };
    // The versions an installed package follows: its own, and the candidate replacing it.
    struct Version {
        bool installed = true;
        std::optional<std::uint32_t> candidate;
    };

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::vector<Atom> atoms_;
    Cps cps_;
    // Visible candidates and installed packages, by cp.
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> visible_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> installed_;
    std::vector<bool> reached_;
    std::vector<bool> taken_;
    std::vector<Step> stack_;
    std::vector<Step> disjunctions_;

    [[nodiscard]] const Store& store() const { return store_.get(); }
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_.get(); }

    [[nodiscard]] bool named(std::uint32_t id) const {
        return std::ranges::any_of(atoms_, [&](const Atom& atom) {
            return matches(store(), store().packages.at(id), atom);
        });
    }

    [[nodiscard]] std::optional<std::uint32_t>
    best(std::string_view cp, const std::function<bool(const Candidate&)>& wanted) const {
        const auto found = visible_.find(cp);
        if (found == visible_.end()) {
            return std::nullopt;
        }
        std::optional<std::uint32_t> best;
        std::optional<egraph::Version> best_version;
        for (const auto index : found->second) {
            const auto& candidate = evaluated().candidates.at(index);
            if (!wanted(candidate)) {
                continue;
            }
            auto version = version_of(evaluated().string(candidate.cpv), cp);
            if (version && (!best_version || vercmp(*version, *best_version) > 0)) {
                best = index;
                best_version = std::move(version);
            }
        }
        return best;
    }

    [[nodiscard]] std::optional<std::uint32_t> best_match(const Atom& atom) const {
        return best(atom.cp, [&](const Candidate& candidate) {
            return matches(store(), evaluated(), candidate, atom);
        });
    }

    // An argument moves to the best version its atoms accept in its slot, and stays without
    // one; any other package moves to its update.
    [[nodiscard]] Version version(std::uint32_t id) const {
        const auto& pkg = store().packages.at(id);
        const auto cp = store().string(pkg.cp);
        if (cps_.contains(cp)) {
            const auto slot = store().string(pkg.slot);
            const auto found = best(cp, [&](const Candidate& candidate) {
                return evaluated().string(candidate.slot) == slot &&
                       std::ranges::any_of(atoms_, [&](const Atom& atom) {
                           return matches(store(), evaluated(), candidate, atom);
                       });
            });
            if (found) {
                if (evaluated().string(evaluated().candidates.at(*found).cpv) ==
                    store().string(pkg.cpv)) {
                    return {};
                }
                return {.installed = false, .candidate = found};
            }
            if (named(id)) {
                return {};
            }
        }
        const auto& own = evaluated().packages.at(id);
        if (!own.target) {
            return {};
        }
        // A --newuse rebuild keeps the version, and its dependencies may be either.
        return {.installed = own.rebuild.count != 0, .candidate = own.target};
    }

    void reach(std::uint32_t id) {
        if (!reached_.at(id)) {
            reached_.at(id) = true;
            stack_.push_back({.candidate = false, .index = id});
        }
    }

    void push(std::uint32_t candidate) {
        if (!taken_.at(candidate)) {
            taken_.at(candidate) = true;
            stack_.push_back({.candidate = true, .index = candidate});
        }
    }

    [[nodiscard]] bool installed_version(std::uint32_t index) const {
        const auto& candidate = evaluated().candidates.at(index);
        const auto found = installed_.find(evaluated().string(candidate.cp));
        return found != installed_.end() &&
               std::ranges::any_of(found->second, [&](std::uint32_t id) {
                   return store().string(store().packages.at(id).cpv) ==
                          evaluated().string(candidate.cpv);
               });
    }

    // A candidate joins the graph: the installed package in its slot decides its own version,
    // and one of the installed version stands for it.
    void take(std::uint32_t index) {
        const auto& candidate = evaluated().candidates.at(index);
        const auto found = installed_.find(evaluated().string(candidate.cp));
        if (found != installed_.end()) {
            for (const auto id : found->second) {
                const auto& pkg = store().packages.at(id);
                if (store().string(pkg.slot) != evaluated().string(candidate.slot)) {
                    continue;
                }
                reach(id);
                if (store().string(pkg.cpv) == evaluated().string(candidate.cpv)) {
                    return;
                }
            }
        }
        push(index);
    }

    [[nodiscard]] const Tables& tables(Step step) const {
        return step.candidate ? static_cast<const Tables&>(evaluated())
                              : static_cast<const Tables&>(store());
    }

    [[nodiscard]] const std::array<Range, dep_kinds.size()>& deps(Step step) const {
        return step.candidate ? evaluated().candidates.at(step.index).deps
                              : store().packages.at(step.index).deps;
    }

    // Outside every ||.
    static bool plain(std::span<const Node> list, std::size_t index) {
        for (auto parent = element(list, index).parent; parent != no_parent;
             parent = element(list, parent).parent) {
            if (element(list, parent).type != NodeType::all_of) {
                return false;
            }
        }
        return true;
    }

    void follow(Step step) {
        if (!step.candidate) {
            const auto [installed, candidate] = version(step.index);
            if (candidate) {
                push(*candidate);
            }
            if (!installed) {
                return;
            }
        }
        const auto& tables = this->tables(step);
        bool disjunctive = false;
        for (const auto range : deps(step)) {
            const auto list = tables.nodes_in(range);
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (!plain(list, i)) {
                    continue;
                }
                const auto& node = element(list, i);
                if (node.type == NodeType::atom) {
                    take_atom(tables, node);
                } else if (node.type == NodeType::any_of) {
                    disjunctive = true;
                }
            }
        }
        if (disjunctive) {
            disjunctions_.push_back(step);
        }
    }

    void choose(Step step) {
        const auto& tables = this->tables(step);
        for (const auto range : deps(step)) {
            const auto list = tables.nodes_in(range);
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (element(list, i).type == NodeType::any_of && plain(list, i)) {
                    take_node(tables, list, i);
                }
            }
        }
    }

    // Without its USE dependencies, as what a USE change could have it merge reaches too.
    [[nodiscard]] static std::optional<Atom> atom(const Tables& tables, const Node& node,
                                                  bool use = false) {
        auto atom = parse_atom(tables.string(node.atom));
        if (!atom) {
            return std::nullopt;
        }
        if (!use) {
            atom->use.clear();
        }
        return std::move(*atom);
    }

    void take_atom(const Tables& tables, const Node& node) {
        const auto ids = tables.ids_in(node.matches);
        if (!ids.empty()) {
            std::ranges::for_each(ids, [this](std::uint32_t id) { reach(id); });
        } else if (const auto wanted = atom(tables, node)) {
            if (const auto found = best_match(*wanted)) {
                // The installed version, which only a USE change lets match, joins the graph
                // rebuilt and holds its slot: emerge leaves its update out.
                if (installed_version(*found)) {
                    push(*found);
                } else {
                    take(*found);
                }
            }
        }
    }

    // How an alternative stands: every atom in the graph, installed, or visible.
    enum class Standing : std::uint8_t { in_graph, installed, visible };

    [[nodiscard]] bool stands(const Tables& tables, std::span<const Node> list, std::size_t index,
                              Standing standing) const {
        const auto& node = element(list, index);
        switch (node.type) {
        case NodeType::weak_blocker:
        case NodeType::strong_blocker:
            return true;
        case NodeType::atom: {
            const auto ids = tables.ids_in(node.matches);
            if (standing == Standing::installed) {
                return !ids.empty();
            }
            if (standing == Standing::in_graph) {
                if (std::ranges::any_of(ids,
                                        [this](std::uint32_t id) { return reached_.at(id); })) {
                    return true;
                }
            } else if (!ids.empty()) {
                return true;
            }
            // emerge takes an alternative a USE change would satisfy only once none is left.
            const auto wanted = atom(tables, node, true);
            if (!wanted) {
                return false;
            }
            if (standing == Standing::visible) {
                return best_match(*wanted).has_value();
            }
            const auto found = visible_.find(wanted->cp);
            return found != visible_.end() &&
                   std::ranges::any_of(found->second, [&](std::uint32_t candidate) {
                       return taken_.at(candidate) &&
                              matches(store(), evaluated(), evaluated().candidates.at(candidate),
                                      *wanted);
                   });
        }
        case NodeType::any_of:
        case NodeType::all_of: {
            const bool any = node.type == NodeType::any_of;
            bool some = false;
            for (std::size_t child = index + 1; child < list.size(); ++child) {
                if (element(list, child).parent != index) {
                    continue;
                }
                some = true;
                if (stands(tables, list, child, standing) == any) {
                    return any;
                }
            }
            return !any || !some;
        }
        }
        return false;
    }

    void take_node(const Tables& tables, std::span<const Node> list, std::size_t index) {
        const auto& node = element(list, index);
        switch (node.type) {
        case NodeType::weak_blocker:
        case NodeType::strong_blocker:
            return;
        case NodeType::atom:
            take_atom(tables, node);
            return;
        case NodeType::all_of:
            for (std::size_t child = index + 1; child < list.size(); ++child) {
                if (element(list, child).parent == index) {
                    take_node(tables, list, child);
                }
            }
            return;
        case NodeType::any_of: {
            std::optional<std::size_t> first;
            for (const auto standing :
                 {Standing::in_graph, Standing::installed, Standing::visible}) {
                for (std::size_t child = index + 1; child < list.size() && !first; ++child) {
                    if (element(list, child).parent == index &&
                        stands(tables, list, child, standing)) {
                        first = child;
                    }
                }
            }
            for (std::size_t child = index + 1; child < list.size() && !first; ++child) {
                if (element(list, child).parent == index) {
                    first = child;
                }
            }
            if (first) {
                take_node(tables, list, *first);
            }
            return;
        }
        }
    }
};

} // namespace

std::expected<Request, std::string> parse_request(const Store& store, const Evaluated& evaluated,
                                                  std::span<const std::string> words,
                                                  const Sets& given) {
    Request request;
    // Filled on the first atom: the cps the stores answer for, and those with every repository's.
    std::optional<std::pair<Cps, Cps>> cps;
    for (const auto& word : words) {
        if (word.starts_with('@')) {
            const auto name = std::string_view{word}.substr(1);
            if (name == "installed") {
                request.installed = true;
                continue;
            }
            if (const auto found = given.find(name); found != given.end()) {
                for (const auto& atom : found->second) {
                    request.arguments.push_back({.set = std::string(name), .atom = atom});
                }
                continue;
            }
            if (!known_set(name)) {
                return std::unexpected(std::format("{}: no such set in the store", word));
            }
            for (const auto& root : store.roots) {
                const auto set = store.string(root.set);
                if (name == "world" ? std::ranges::contains(world_sets, set) : set == name) {
                    request.arguments.push_back(
                        {.set = std::string(set), .atom = std::string(store.string(root.atom))});
                }
            }
            continue;
        }
        if (!cps) {
            auto known = evaluated_cps(store, evaluated);
            auto all = known;
            for (const auto id : evaluated.ids_in(evaluated.repository_cps)) {
                all.emplace(evaluated.string(id));
            }
            cps.emplace(std::move(known), std::move(all));
        }
        const auto& [known, all] = *cps;
        auto text = word.starts_with('!') ? std::expected<std::string, std::string>{word}
                                          : with_category(word, all);
        if (!text) {
            return std::unexpected(text.error());
        }
        const auto atom = parse_atom(*text);
        if (atom && !known.contains(atom->cp) && all.contains(atom->cp)) {
            if (!std::ranges::contains(request.unevaluated, atom->cp)) {
                request.unevaluated.push_back(atom->cp);
            }
        } else if (auto error = refusal(store, evaluated, *text)) {
            return std::unexpected(std::move(*error));
        }
        request.arguments.push_back({.set = "", .atom = std::move(*text)});
    }
    std::ranges::sort(request.unevaluated);
    return request;
}

std::vector<bool> request_reach(const Store& store, const Evaluated& evaluated,
                                const Request& request) {
    return Reach(store, evaluated, request).walk();
}

} // namespace egraph
