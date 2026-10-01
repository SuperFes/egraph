#include "use_changes.hpp"

#include "atom.hpp"
#include "use_reduce.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace egraph {

namespace {

std::uint32_t size32(std::size_t size) {
    return static_cast<std::uint32_t>(size);
}

// Appends strings to a table, each once.
class Interner {
  public:
    explicit Interner(Tables& tables) : tables_(tables) {
        for (std::uint32_t id = 0; id < tables.strings.size(); ++id) {
            ids_.try_emplace(std::string{tables.string(id)}, id);
        }
    }

    std::uint32_t operator()(const std::string& text) {
        if (const auto found = ids_.find(text); found != ids_.end()) {
            return found->second;
        }
        auto& tables = tables_.get();
        const auto id = size32(tables.strings.size());
        tables.strings.push_back(
            {.first = size32(tables.pool.size()), .count = size32(text.size())});
        tables.pool += text;
        ids_.emplace(text, id);
        return id;
    }

  private:
    std::reference_wrapper<Tables> tables_;
    std::unordered_map<std::string, std::uint32_t> ids_;
};

// The installed packages the node's atom matches, a blocker's without its "!"s.
std::vector<std::uint32_t> installed_matches(const Store& installed, const ReducedNode& node) {
    std::vector<std::uint32_t> found;
    if (node.type == NodeType::any_of || node.type == NodeType::all_of) {
        return found;
    }
    const auto text = std::string_view{node.text};
    const auto atom = parse_atom(text.substr(std::min(text.find_first_not_of('!'), text.size())));
    if (!atom) {
        return found;
    }
    for (std::uint32_t id = 0; id < installed.packages.size(); ++id) {
        if (matches(installed, installed.packages.at(id), *atom)) {
            found.push_back(id);
        }
    }
    return found;
}

} // namespace

Evaluated with_use_changes(Evaluated evaluated, const Store& installed,
                           std::span<const UseChange> changes) {
    if (changes.empty()) {
        return evaluated;
    }
    Interner intern(evaluated);
    for (const auto& change : changes) {
        auto& candidate = evaluated.candidates.at(change.candidate);
        std::set<std::string> use;
        for (const auto id : evaluated.ids_in(candidate.use)) {
            use.emplace(evaluated.string(id));
        }
        for (const auto& [flag, on] : change.flags) {
            if (on) {
                use.insert(flag);
            } else {
                use.erase(flag);
            }
        }
        // Copied first: interning grows the pool the tables' views point into.
        std::array<std::vector<std::string>, dep_kinds.size()> tokens;
        for (std::size_t kind = 0; kind < tokens.size(); ++kind) {
            for (const auto id : evaluated.ids_in(candidate.tokens.at(kind))) {
                tokens.at(kind).emplace_back(evaluated.string(id));
            }
        }
        std::vector<std::uint32_t> use_ids;
        use_ids.reserve(use.size());
        for (const auto& flag : use) {
            use_ids.push_back(intern(flag));
        }
        candidate.use = {.first = size32(evaluated.ids.size()), .count = size32(use_ids.size())};
        std::ranges::copy(use_ids, std::back_inserter(evaluated.ids));
        const std::set<std::string_view> enabled(use.begin(), use.end());
        for (std::size_t kind = 0; kind < tokens.size(); ++kind) {
            const std::vector<std::string_view> views(tokens.at(kind).begin(),
                                                      tokens.at(kind).end());
            const auto first = size32(evaluated.nodes.size());
            for (const auto& reduced :
                 reduce_dependencies(views, enabled, candidate.empty_groups_true)) {
                const auto found = installed_matches(installed, reduced);
                Node node{.type = reduced.type,
                          .parent = reduced.parent,
                          .atom = intern(reduced.text),
                          .matches = {.first = size32(evaluated.ids.size()),
                                      .count = size32(found.size())}};
                std::ranges::copy(found, std::back_inserter(evaluated.ids));
                evaluated.nodes.push_back(node);
            }
            candidate.deps.at(kind) = {.first = first,
                                       .count = size32(evaluated.nodes.size()) - first};
        }
    }
    return evaluated;
}

} // namespace egraph
