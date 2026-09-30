#include "use_reduce.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <utility>

namespace egraph {

namespace {

// A term of use_reduce's stacks: a token ("||", "flag?" or an atom) or a list.
struct Term {
    bool is_list = false;
    std::string token;
    std::vector<Term> list;
};

Term token_term(std::string text) {
    return {.is_list = false, .token = std::move(text), .list = {}};
}

bool is_token(const Term& term, std::string_view text) {
    return !term.is_list && term.token == text;
}

void extend(std::vector<Term>& into, std::vector<Term> terms) {
    std::ranges::move(terms, std::back_inserter(into));
}

class Reducer {
  public:
    Reducer(const std::set<std::string_view>& enabled, bool empty_true)
        : enabled_(enabled), empty_true_(empty_true) {}

    std::vector<Term> reduce(std::span<const std::string_view> tokens) {
        stack_.assign(1, {});
        for (const auto token : tokens) {
            if (token == "(") {
                stack_.emplace_back();
            } else if (token == ")") {
                close();
            } else if (token == "||" || token.ends_with('?')) {
                stack_.back().push_back(token_term(std::string{token}));
            } else {
                stack_.back().push_back(
                    token_term(evaluate_use_conditionals(token, enabled_.get())));
            }
        }
        return std::move(stack_.front());
    }

  private:
    std::reference_wrapper<const std::set<std::string_view>> enabled_;
    bool empty_true_;
    std::vector<std::vector<Term>> stack_;

    [[nodiscard]] bool active(std::string_view conditional) const {
        const bool negated = conditional.starts_with('!');
        const auto flag =
            conditional.substr(negated ? 1 : 0, conditional.size() - (negated ? 2 : 1));
        return enabled_.get().contains(flag) != negated;
    }

    // Whether the level (possibly -1, none) ends in "||".
    [[nodiscard]] bool ends_in_any_of(std::ptrdiff_t level) const {
        if (level < 0) {
            return false;
        }
        const auto& terms = stack_.at(static_cast<std::size_t>(level));
        return !terms.empty() && is_token(terms.back(), "||");
    }

    // The nearest level at or below this one ending in "||", through conditionals only; -1
    // when another token comes first.
    [[nodiscard]] std::ptrdiff_t last_any_of_level(std::ptrdiff_t level) const {
        for (; level >= 0; --level) {
            const auto& terms = stack_.at(static_cast<std::size_t>(level));
            if (!terms.empty() && !terms.back().is_list) {
                if (terms.back().token == "||") {
                    return level;
                }
                if (!terms.back().token.ends_with('?')) {
                    return -1;
                }
            }
        }
        return -1;
    }

    void close() {
        auto group = std::move(stack_.back());
        stack_.pop_back();
        const auto level = static_cast<std::ptrdiff_t>(stack_.size()) - 1;
        auto& outer = stack_.back();
        const bool single =
            group.size() == 1 || (group.size() == 2 && is_token(group.front(), "||"));
        bool ignore = false;
        if (!outer.empty() && !outer.back().is_list) {
            if (outer.back().token == "||" && group.empty()) {
                if (!empty_true_) {
                    group.push_back(token_term("__const__/empty-any-of"));
                }
                outer.pop_back();
            } else if (outer.back().token.ends_with('?')) {
                ignore = !active(outer.back().token);
                outer.pop_back();
            }
        }
        if (group.empty() || ignore) {
            return;
        }
        // An empty level never ends in "||", so use_reduce's case for one falls to the last.
        if (!ends_in_any_of(level - 1) && !ends_in_any_of(level)) {
            extend(outer, std::move(group));
        } else if (single && ends_in_any_of(level)) {
            outer.pop_back();
            append(level, std::move(group), single);
        } else if (ends_in_any_of(level) && ends_in_any_of(level - 1)) {
            outer.pop_back();
            extend(outer, std::move(group));
        } else {
            append(level, std::move(group), single);
        }
    }

    // use_reduce's special_append: extends rather than appends where that loses no meaning.
    void append(std::ptrdiff_t level, std::vector<Term> group, bool single) {
        auto& outer = stack_.at(static_cast<std::size_t>(level));
        if (!single) {
            outer.push_back({.is_list = true, .token = {}, .list = std::move(group)});
        } else if (is_token(group.front(), "||") && ends_in_any_of(level - 1)) {
            extend(outer, std::move(group.at(1).list));
        } else if (group.size() == 1 && group.front().is_list) {
            if (last_any_of_level(level - 1) == -1) {
                extend(outer, std::move(group.front().list));
            } else {
                outer.push_back(std::move(group.front()));
            }
        } else {
            extend(outer, std::move(group));
        }
    }
};

// installed._tree: "||" then its list is an any-of node, a list an all-of node.
void lay_out(const std::vector<Term>& terms, std::uint32_t parent,
             std::vector<ReducedNode>& nodes) {
    for (std::size_t i = 0; i < terms.size(); ++i) {
        const auto& term = terms.at(i);
        const auto index = static_cast<std::uint32_t>(nodes.size());
        if (is_token(term, "||")) {
            nodes.push_back({.type = NodeType::any_of, .parent = parent, .text = {}});
            lay_out(terms.at(++i).list, index, nodes);
        } else if (term.is_list) {
            nodes.push_back({.type = NodeType::all_of, .parent = parent, .text = {}});
            lay_out(term.list, index, nodes);
        } else {
            const auto type = term.token.starts_with("!!")  ? NodeType::strong_blocker
                              : term.token.starts_with('!') ? NodeType::weak_blocker
                                                            : NodeType::atom;
            nodes.push_back({.type = type, .parent = parent, .text = term.token});
        }
    }
}

} // namespace

std::vector<ReducedNode> reduce_dependencies(std::span<const std::string_view> tokens,
                                             const std::set<std::string_view>& enabled,
                                             bool empty_true) {
    std::vector<ReducedNode> nodes;
    lay_out(Reducer{enabled, empty_true}.reduce(tokens), no_parent, nodes);
    return nodes;
}

std::string evaluate_use_conditionals(std::string_view atom,
                                      const std::set<std::string_view>& enabled) {
    const auto open = atom.find('[');
    if (open == std::string_view::npos || !atom.ends_with(']')) {
        return std::string{atom};
    }
    const auto uses = atom.substr(open + 1, atom.size() - open - 2);
    std::vector<std::string_view> deps;
    for (std::size_t start = 0; start <= uses.size();) {
        const auto comma = std::min(uses.find(',', start), uses.size());
        deps.push_back(uses.substr(start, comma - start));
        start = comma + 1;
    }
    if (std::ranges::none_of(
            deps, [](std::string_view dep) { return dep.ends_with('?') || dep.ends_with('='); })) {
        return std::string{atom};
    }
    std::vector<std::string> evaluated;
    for (const auto dep : deps) {
        const bool conditional = dep.ends_with('?');
        if (!conditional && !dep.ends_with('=')) {
            evaluated.emplace_back(dep);
            continue;
        }
        const bool negated = dep.starts_with('!');
        auto flag = dep.substr(negated ? 1 : 0, dep.size() - (negated ? 2 : 1));
        std::string_view fallback;
        if (flag.ends_with("(+)") || flag.ends_with("(-)")) {
            fallback = flag.substr(flag.size() - 3);
            flag.remove_suffix(3);
        }
        const bool on = enabled.contains(flag);
        // x? keeps x when on; !x? keeps -x when off; x= and !x= always keep one.
        if (conditional && on == negated) {
            continue;
        }
        const bool wanted = conditional ? on : on != negated;
        evaluated.push_back(std::format("{}{}{}", wanted ? "" : "-", flag, fallback));
    }
    // The slot, sub-slot and operator stay; a repository goes, as portage rebuilds the atom
    // from remove_slot().
    const auto base_end = std::min(atom.find(':'), open);
    std::string text{atom.substr(0, base_end)};
    if (base_end < open && atom.substr(base_end, 2) != "::") {
        const auto slot = atom.substr(base_end, open - base_end);
        text += slot.substr(0, std::min(slot.find("::"), slot.size()));
    }
    if (!evaluated.empty()) {
        text += '[';
        for (std::size_t i = 0; i < evaluated.size(); ++i) {
            text += (i == 0 ? "" : ",") + evaluated.at(i);
        }
        text += ']';
    }
    return text;
}

} // namespace egraph
