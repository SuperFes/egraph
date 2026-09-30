#include "required_use.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace egraph {

namespace {

// A node of portage's _RequiredUseBranch/_RequiredUseLeaf tree, in an arena.
struct UseNode {
    bool leaf = false;
    // A leaf's token, or a branch's operator: "||", "^^", "??", "flag?", or empty for a group.
    std::string_view text;
    bool satisfied = false;
    // The root, index 0, has none of its own.
    std::size_t parent = 0;
    std::vector<std::size_t> children;
};

bool valid_operator(std::string_view text) {
    return text == "||" || text == "^^" || text == "??";
}

class Tree {
  public:
    Tree(const std::set<std::string_view>& enabled, bool empty_true)
        : enabled_(enabled), empty_true_(empty_true), nodes_(1) {}

    RequiredUse check(std::span<const std::string_view> tokens) {
        // Each level's terms: an operator not yet closed, or a closed term's truth.
        std::vector<std::vector<std::variant<bool, std::string_view>>> stack(1);
        std::size_t node = 0;
        bool need_bracket = false;
        for (const auto token : tokens) {
            if (token == "(") {
                if (!need_bracket) {
                    node = add(node, false, {});
                }
                need_bracket = false;
                stack.emplace_back();
            } else if (token == ")") {
                const auto terms = std::move(stack.back());
                stack.pop_back();
                auto& level = stack.back();
                std::optional<std::string_view> op;
                if (!level.empty() && std::holds_alternative<std::string_view>(level.back())) {
                    const auto last = std::get<std::string_view>(level.back());
                    level.pop_back();
                    if (valid_operator(last) || active(last.substr(0, last.size() - 1))) {
                        op = last;
                        const bool satisfied = holds(last, terms);
                        level.emplace_back(satisfied);
                        nodes_.at(node).satisfied = satisfied;
                    } else {
                        nodes_.at(node).satisfied = true;
                        node = detach(node);
                        continue;
                    }
                }
                if (!op) {
                    const bool satisfied = all(terms);
                    nodes_.at(node).satisfied = satisfied;
                    if (!terms.empty()) {
                        level.emplace_back(satisfied);
                    }
                    if (nodes_.at(node).children.size() <= 1 ||
                        !valid_operator(nodes_.at(parent(node)).text)) {
                        flatten(node);
                    }
                } else if (nodes_.at(node).children.empty()) {
                    detach(node);
                } else if (nodes_.at(node).children.size() == 1 && valid_operator(*op)) {
                    const auto up = detach(node);
                    const auto only = nodes_.at(node).children.front();
                    nodes_.at(up).children.push_back(only);
                    if (!nodes_.at(only).leaf) {
                        nodes_.at(only).parent = up;
                        node = only;
                        if (nodes_.at(node).text.empty() && !valid_operator(nodes_.at(up).text)) {
                            flatten(node);
                        }
                    }
                }
                node = parent(node);
            } else if (valid_operator(token) || token.ends_with('?')) {
                need_bracket = true;
                stack.back().emplace_back(token);
                node = add(node, false, token);
            } else {
                const bool satisfied = active(token);
                stack.back().emplace_back(satisfied);
                nodes_.at(add(node, true, token)).satisfied = satisfied;
            }
        }
        return {.satisfied = all(stack.front()), .unsatisfied = text(0)};
    }

  private:
    std::reference_wrapper<const std::set<std::string_view>> enabled_;
    bool empty_true_;
    std::vector<UseNode> nodes_;

    [[nodiscard]] bool active(std::string_view token) const {
        const bool negated = token.starts_with('!');
        return enabled_.get().contains(negated ? token.substr(1) : token) != negated;
    }

    [[nodiscard]] static bool all(const std::vector<std::variant<bool, std::string_view>>& terms) {
        return std::ranges::none_of(terms, [](const auto& term) {
            return std::holds_alternative<bool>(term) && !std::get<bool>(term);
        });
    }

    [[nodiscard]] bool holds(std::string_view op,
                             const std::vector<std::variant<bool, std::string_view>>& terms) const {
        if (terms.empty() && empty_true_) {
            return true;
        }
        const auto count = std::ranges::count_if(terms, [](const auto& term) {
            return std::holds_alternative<bool>(term) && std::get<bool>(term);
        });
        if (op == "||") {
            return count > 0;
        }
        if (op == "^^") {
            return count == 1;
        }
        if (op == "??") {
            return count <= 1;
        }
        return all(terms);
    }

    [[nodiscard]] std::size_t parent(std::size_t node) const { return nodes_.at(node).parent; }

    std::size_t add(std::size_t parent, bool leaf, std::string_view text) {
        const auto index = nodes_.size();
        nodes_.push_back(
            {.leaf = leaf, .text = text, .satisfied = false, .parent = parent, .children = {}});
        nodes_.at(parent).children.push_back(index);
        return index;
    }

    // Takes the node, its parent's last child, off its parent; returns the parent.
    std::size_t detach(std::size_t node) {
        const auto up = parent(node);
        nodes_.at(up).children.pop_back();
        return up;
    }

    // Replaces the node, its parent's last child, with its children.
    void flatten(std::size_t node) {
        const auto up = detach(node);
        for (const auto child : nodes_.at(node).children) {
            nodes_.at(up).children.push_back(child);
            if (!nodes_.at(child).leaf) {
                nodes_.at(child).parent = up;
            }
        }
    }

    [[nodiscard]] std::string text(std::size_t index) const {
        const auto& node = nodes_.at(index);
        if (node.leaf) {
            return std::string{node.text};
        }
        std::vector<std::string> tokens;
        if (!node.text.empty()) {
            tokens.emplace_back(node.text);
        }
        if (index != 0) {
            tokens.emplace_back("(");
        }
        bool whole = valid_operator(node.text);
        for (auto at = index; at != 0 && !whole;) {
            at = nodes_.at(at).parent;
            whole = valid_operator(nodes_.at(at).text);
        }
        for (const auto child : node.children) {
            if (whole || !nodes_.at(child).satisfied) {
                tokens.push_back(text(child));
            }
        }
        if (index != 0) {
            tokens.emplace_back(")");
        }
        std::string joined;
        for (const auto& token : tokens) {
            if (&token != &tokens.front()) {
                joined += ' ';
            }
            joined += token;
        }
        return joined;
    }
};

} // namespace

RequiredUse check_required_use(std::span<const std::string_view> tokens,
                               const std::set<std::string_view>& enabled, bool empty_true) {
    return Tree{enabled, empty_true}.check(tokens);
}

std::string human_readable_required_use(std::string_view constraints) {
    std::string text{constraints};
    for (const auto& [from, to] :
         {std::pair<std::string_view, std::string_view>{"^^", "exactly-one-of"},
          {"||", "any-of"},
          {"??", "at-most-one-of"}}) {
        for (auto at = text.find(from); at != std::string::npos;
             at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }
    return text;
}

} // namespace egraph
