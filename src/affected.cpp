#include "affected.hpp"

#include "atom.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace egraph {

namespace {

using Json = nlohmann::json;

constexpr std::string_view soname_kind = "SONAME";

// The atom a dependency string holds without its blocker marks and USE dependencies, parsed.
std::optional<Atom> loose_atom(std::string_view text) {
    while (text.starts_with('!')) {
        text.remove_prefix(1);
    }
    text = text.substr(0, text.find('['));
    auto atom = parse_atom(text);
    return atom ? std::optional{std::move(*atom)} : std::nullopt;
}

// The store's packages by cpv and cp, and what each atom string matches, worked out once.
class Index {
  public:
    explicit Index(const Store& store) : store_(store) {
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            const auto& pkg = store.packages.at(id);
            by_cpv_.emplace(store.string(pkg.cpv), id);
            by_cp_[std::string{store.string(pkg.cp)}].push_back(id);
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view cpv) const {
        const auto found = by_cpv_.find(cpv);
        return found == by_cpv_.end() ? std::nullopt : std::optional{found->second};
    }

    // What an atom from a dependency string matches, cached by the string.
    const std::vector<std::uint32_t>& matches(std::string_view text) {
        const auto cached = matches_.find(text);
        if (cached != matches_.end()) {
            return cached->second;
        }
        std::vector<std::uint32_t> found;
        if (const auto atom = loose_atom(text)) {
            if (const auto candidates = by_cp_.find(atom->cp); candidates != by_cp_.end()) {
                std::ranges::copy_if(
                    candidates->second, std::back_inserter(found), [&](std::uint32_t id) {
                        const auto& store = store_.get();
                        return egraph::matches(store, store.packages.at(id), *atom);
                    });
            }
        }
        return matches_.emplace(std::string{text}, std::move(found)).first->second;
    }

  private:
    std::reference_wrapper<const Store> store_;
    std::map<std::string, std::uint32_t, std::less<>> by_cpv_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> matches_;
};

bool is_dependency(NodeType type) {
    return type == NodeType::atom || type == NodeType::weak_blocker ||
           type == NodeType::strong_blocker;
}

std::vector<std::string> cpvs(const Store& store, const std::set<std::uint32_t>& ids) {
    std::vector<std::string> found;
    found.reserve(ids.size());
    for (const auto id : ids) {
        found.emplace_back(store.string(store.packages.at(id).cpv));
    }
    std::ranges::sort(found);
    return found;
}

std::set<std::uint32_t> reachable(const Store& store, Index& index,
                                  const AffectedRequest& request) {
    std::vector<std::size_t> kinds;
    bool sonames = false;
    for (const auto& kind : request.kinds) {
        if (kind == soname_kind) {
            sonames = true;
        } else if (const auto found = std::ranges::find(dep_kinds, kind);
                   found != dep_kinds.end()) {
            kinds.push_back(static_cast<std::size_t>(found - dep_kinds.begin()));
        }
    }
    std::set<std::uint32_t> seen;
    std::vector<std::uint32_t> stack;
    for (const auto& seed : request.seeds) {
        if (const auto id = index.find(seed); id && seen.insert(*id).second) {
            stack.push_back(*id);
        }
    }
    const auto visit = [&](std::uint32_t child) {
        if (seen.insert(child).second) {
            stack.push_back(child);
        }
    };
    while (!stack.empty()) {
        const auto& pkg = store.packages.at(stack.back());
        stack.pop_back();
        for (const auto kind : kinds) {
            // Blockers are constraints, not dependencies, and are not followed.
            for (const auto& node : store.nodes_in(pkg.deps.at(kind))) {
                if (node.type == NodeType::atom) {
                    std::ranges::for_each(index.matches(store.string(node.atom)), visit);
                }
            }
        }
        if (sonames) {
            for (const auto& required : store.required_in(pkg.required)) {
                std::ranges::for_each(store.ids_in(required.providers), visit);
            }
        }
    }
    return seen;
}

} // namespace

std::expected<AffectedRequest, std::string> parse_request(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("the request is not a JSON object");
    }
    AffectedRequest request;
    const std::array<std::pair<std::string_view, std::vector<std::string>*>, 5> fields{{
        {"kinds", &request.kinds},
        {"seeds", &request.seeds},
        {"changed", &request.changed},
        {"replaced", &request.replaced},
        {"blockers", &request.blockers},
    }};
    for (const auto& [name, field] : fields) {
        const auto found = json.find(name);
        if (found == json.end()) {
            continue;
        }
        if (!found->is_array() || !std::ranges::all_of(*found, &Json::is_string)) {
            return std::unexpected(std::format("{}: not a list of strings", name));
        }
        for (const auto& value : *found) {
            field->push_back(value.get<std::string>());
        }
    }
    for (const auto& kind : request.kinds) {
        if (kind != soname_kind && std::ranges::find(dep_kinds, kind) == dep_kinds.end()) {
            return std::unexpected(std::format("kinds: no such dependency kind: {}", kind));
        }
    }
    return request;
}

std::vector<std::uint32_t> loose_matches(const Store& store, std::string_view atom) {
    Index index{store};
    return index.matches(atom);
}

AffectedAnswer affected(const Store& store, const AffectedRequest& request) {
    Index index{store};

    std::set<std::uint32_t> blocked;
    for (const auto& blocker : request.blockers) {
        blocked.insert_range(index.matches(blocker));
    }

    // Every package with a dependency, blockers included, naming one of the cps.
    std::set<std::string, std::less<>> cps{request.changed.begin(), request.changed.end()};
    for (const auto id : blocked) {
        cps.emplace(store.string(store.packages.at(id).cp));
    }
    std::set<std::uint32_t> affected = blocked;
    std::map<std::uint32_t, std::optional<std::string>> cp_of_atom;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        for (const auto& range : store.packages.at(id).deps) {
            for (const auto& node : store.nodes_in(range)) {
                if (!is_dependency(node.type)) {
                    continue;
                }
                auto [cached, added] = cp_of_atom.try_emplace(node.atom);
                if (added) {
                    if (const auto atom = loose_atom(store.string(node.atom))) {
                        cached->second = atom->cp;
                    }
                }
                if (cached->second && cps.contains(*cached->second)) {
                    affected.insert(id);
                }
            }
        }
    }

    // Consumers of every soname a replaced package provides.
    std::set<std::pair<std::string_view, std::string_view>> lost;
    for (const auto& cpv : request.replaced) {
        if (const auto id = index.find(cpv)) {
            for (const auto& [category, soname] : store.pairs_in(store.packages.at(*id).provided)) {
                lost.emplace(store.string(category), store.string(soname));
            }
        }
    }
    if (!lost.empty()) {
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            for (const auto& required : store.required_in(store.packages.at(id).required)) {
                if (lost.contains(
                        {store.string(required.category), store.string(required.soname)})) {
                    affected.insert(id);
                }
            }
        }
    }

    return {.reachable = cpvs(store, reachable(store, index, request)),
            .blocked = cpvs(store, blocked),
            .affected = cpvs(store, affected)};
}

std::string to_json(const AffectedAnswer& answer) {
    const Json json{{"reachable", answer.reachable},
                    {"blocked", answer.blocked},
                    {"affected", answer.affected}};
    return json.dump() + "\n";
}

} // namespace egraph
