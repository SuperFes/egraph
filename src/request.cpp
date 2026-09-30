#include "request.hpp"

#include "atom.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
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

} // namespace

std::expected<Request, std::string> parse_request(const Store& store, const Evaluated& evaluated,
                                                  std::span<const std::string> words) {
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
    std::vector<Atom> atoms;
    std::set<std::string, std::less<>> cps;
    for (const auto& argument : request.arguments) {
        if (auto atom = parse_atom(argument.atom)) {
            cps.insert(atom->cp);
            atoms.push_back(std::move(*atom));
        }
    }
    std::vector<std::uint32_t> stack;
    const auto push_deps = [&](const Tables& tables, const auto& deps) {
        for (const auto range : deps) {
            for (const auto& node : tables.nodes_in(range)) {
                if (node.type == NodeType::atom) {
                    std::ranges::copy(tables.ids_in(node.matches), std::back_inserter(stack));
                }
            }
        }
    };
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        const auto& pkg = store.packages.at(id);
        if (std::ranges::any_of(atoms,
                                [&](const Atom& atom) { return matches(store, pkg, atom); })) {
            stack.push_back(id);
        }
    }
    for (const auto& candidate : evaluated.candidates) {
        if (!candidate.visible() || !cps.contains(evaluated.string(candidate.cp)) ||
            std::ranges::none_of(atoms, [&](const Atom& atom) {
                return matches(store, evaluated, candidate, atom);
            })) {
            continue;
        }
        push_deps(evaluated, candidate.deps);
        // The installed version it would replace.
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            const auto& pkg = store.packages.at(id);
            if (store.string(pkg.cp) == evaluated.string(candidate.cp) &&
                store.string(pkg.slot) == evaluated.string(candidate.slot)) {
                stack.push_back(id);
            }
        }
    }
    std::vector<bool> reach(store.packages.size());
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (reach.at(id)) {
            continue;
        }
        reach.at(id) = true;
        const auto& pkg = store.packages.at(id);
        push_deps(store, pkg.deps);
        const auto target = evaluated.packages.at(id).target;
        if (!target) {
            continue;
        }
        // An argument moves only to a version its atom accepts.
        const auto& candidate = evaluated.candidates.at(*target);
        const auto named = [&](const Atom& atom) { return matches(store, pkg, atom); };
        const auto accepts = [&](const Atom& atom) {
            return matches(store, evaluated, candidate, atom);
        };
        if (std::ranges::none_of(atoms, named) || std::ranges::any_of(atoms, accepts)) {
            push_deps(evaluated, candidate.deps);
        }
    }
    return reach;
}

} // namespace egraph
