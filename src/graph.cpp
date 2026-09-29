#include "graph.hpp"

#include "atom.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace egraph {

namespace {

std::uint32_t size32(std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("graph size exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(size);
}

template <class T> const T& element(std::span<const T> items, std::size_t index) {
    return items.subspan(index).front();
}

// Groups edges by one endpoint: ranges[i] slices the edges whose endpoint is package i.
std::vector<Range> ranges_by(const std::vector<Edge>& sorted, std::size_t packages,
                             std::uint32_t Edge::* endpoint) {
    std::vector<Range> ranges(packages);
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        auto& range = ranges.at(sorted.at(i).*endpoint);
        if (range.count == 0) {
            range.first = size32(i);
        }
        ++range.count;
    }
    return ranges;
}

} // namespace

std::span<const Edge> Graph::deps(std::uint32_t package) const {
    const auto range = forward_of.at(package);
    return std::span{forward}.subspan(range.first, range.count);
}

std::span<const Edge> Graph::rdeps(std::uint32_t package) const {
    const auto range = reverse_of.at(package);
    return std::span{reverse}.subspan(range.first, range.count);
}

std::vector<bool> choices(std::span<const Node> nodes) {
    std::vector<bool> inside(nodes.size(), false);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto parent = element(nodes, i).parent;
        if (parent != no_parent) {
            inside.at(i) = element(nodes, parent).type == NodeType::any_of || inside.at(parent);
        }
    }
    return inside;
}

std::vector<bool> satisfied(std::span<const Node> nodes) {
    return satisfied(nodes,
                     [&nodes](std::size_t i) { return element(nodes, i).matches.count != 0; });
}

std::vector<bool> satisfied(std::span<const Node> nodes,
                            const std::function<bool(std::size_t)>& atom_satisfied) {
    // Children follow their parent, so a backward pass sees every child before its parent.
    // Groups start from the answer for no children: an empty any-of is satisfied, and so is an
    // empty all-of.
    std::vector<bool> result(nodes.size(), true);
    std::vector<bool> any_child(nodes.size(), false);
    std::vector<bool> all_children(nodes.size(), true);
    std::vector<bool> has_children(nodes.size(), false);
    for (std::size_t i = nodes.size(); i-- > 0;) {
        const auto& node = element(nodes, i);
        switch (node.type) {
        case NodeType::atom:
            result.at(i) = atom_satisfied(i);
            break;
        case NodeType::any_of:
            result.at(i) = !has_children.at(i) || any_child.at(i);
            break;
        case NodeType::all_of:
            result.at(i) = all_children.at(i);
            break;
        case NodeType::weak_blocker:
        case NodeType::strong_blocker:
            result.at(i) = true;
            break;
        }
        if (node.parent != no_parent) {
            has_children.at(node.parent) = true;
            if (result.at(i)) {
                any_child.at(node.parent) = true;
            } else {
                all_children.at(node.parent) = false;
            }
        }
    }
    return result;
}

Graph build_graph(const Store& store) {
    Graph graph;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        const auto& pkg = store.packages.at(id);
        const auto first = graph.forward.size();
        for (std::uint32_t kind = 0; kind < dep_kinds.size(); ++kind) {
            const auto nodes = store.nodes_in(pkg.deps.at(kind));
            const auto inside = choices(nodes);
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const auto& node = element(nodes, i);
                if (node.type != NodeType::atom) {
                    continue;
                }
                for (const auto child : store.ids_in(node.matches)) {
                    graph.forward.push_back({.parent = id,
                                             .child = child,
                                             .kind = kind,
                                             .atom = node.atom,
                                             .choice = inside.at(i)});
                }
            }
        }
        // The same atom can appear more than once; the oracle counts the edge once.
        const auto begin = graph.forward.begin() + static_cast<std::ptrdiff_t>(first);
        std::sort(begin, graph.forward.end());
        graph.forward.erase(std::unique(begin, graph.forward.end()), graph.forward.end());
    }
    graph.forward_of = ranges_by(graph.forward, store.packages.size(), &Edge::parent);
    graph.reverse = graph.forward;
    std::ranges::sort(graph.reverse, [](const Edge& a, const Edge& b) {
        return std::tie(a.child, a.parent, a.kind, a.atom, a.choice) <
               std::tie(b.child, b.parent, b.kind, b.atom, b.choice);
    });
    graph.reverse_of = ranges_by(graph.reverse, store.packages.size(), &Edge::child);
    return graph;
}

std::expected<std::vector<std::uint32_t>, std::string> resolve(const Store& store,
                                                               std::string_view argument) {
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        if (store.string(store.packages.at(id).cpv) == argument) {
            return std::vector<std::uint32_t>{id};
        }
    }
    const auto atom = parse_atom(argument);
    if (!atom) {
        return std::unexpected(atom.error());
    }
    std::vector<std::uint32_t> found;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        if (matches(store, store.packages.at(id), *atom)) {
            found.push_back(id);
        }
    }
    return found;
}

} // namespace egraph
