#include "query.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <ostream>
#include <set>
#include <string_view>

namespace egraph {

namespace {

template <class T> const T& element(std::span<const T> items, std::size_t index) {
    return items.subspan(index).front();
}

void sorted_unique(std::vector<std::string>& lines) {
    std::ranges::sort(lines);
    const auto duplicates = std::ranges::unique(lines);
    lines.erase(duplicates.begin(), duplicates.end());
}

std::string dot_id(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string edge_line(const Store& store, const Edge& edge) {
    return std::format("{}\t{}\t{}\t{}{}", store.string(store.packages.at(edge.parent).cpv),
                       dep_kinds.at(edge.kind), store.string(edge.atom),
                       store.string(store.packages.at(edge.child).cpv),
                       edge.choice ? "\tany-of" : "");
}

std::vector<std::string> edge_lines(const Store& store, std::span<const Edge> edges) {
    std::vector<std::string> lines;
    lines.reserve(edges.size());
    for (const auto& edge : edges) {
        lines.push_back(edge_line(store, edge));
    }
    sorted_unique(lines);
    return lines;
}

std::vector<std::string> soname_users(const Store& store, std::string_view soname, bool providers) {
    std::vector<std::string> lines;
    for (const auto& pkg : store.packages) {
        const auto cpv = store.string(pkg.cpv);
        if (providers) {
            for (const auto& provided : store.pairs_in(pkg.provided)) {
                if (store.string(provided.second) == soname) {
                    lines.push_back(std::format("{}\t{}", cpv, store.string(provided.first)));
                }
            }
        } else {
            for (const auto& required : store.required_in(pkg.required)) {
                if (store.string(required.soname) == soname) {
                    lines.push_back(std::format("{}\t{}", cpv, store.string(required.category)));
                }
            }
        }
    }
    sorted_unique(lines);
    return lines;
}

std::string render(const Store& store, std::span<const Node> nodes, std::size_t index) {
    const auto& node = element(nodes, index);
    if (node.type != NodeType::any_of && node.type != NodeType::all_of) {
        return std::string{store.string(node.atom)};
    }
    std::string out = node.type == NodeType::any_of ? "|| (" : "(";
    for (std::size_t child = index + 1; child < nodes.size(); ++child) {
        if (element(nodes, child).parent == index) {
            out += ' ';
            out += render(store, nodes, child);
        }
    }
    out += " )";
    return out;
}

std::vector<Unsatisfied> unsatisfied(const Store& store, std::uint32_t package) {
    std::vector<Unsatisfied> found;
    const auto& pkg = store.packages.at(package);
    for (std::uint32_t kind = 0; kind < dep_kinds.size(); ++kind) {
        const auto nodes = store.nodes_in(pkg.deps.at(kind));
        const auto ok = satisfied(nodes);
        for (std::uint32_t i = 0; i < nodes.size(); ++i) {
            if (element(nodes, i).parent == no_parent && !ok.at(i)) {
                found.push_back({.package = package, .kind = kind, .node = i});
            }
        }
    }
    return found;
}

std::string render(const Store& store, const Unsatisfied& dependency) {
    const auto& pkg = store.packages.at(dependency.package);
    return render(store, store.nodes_in(pkg.deps.at(dependency.kind)), dependency.node);
}

std::vector<std::string> broken(const Store& store) {
    std::vector<std::string> lines;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        for (const auto& dependency : unsatisfied(store, id)) {
            lines.push_back(std::format("{}\t{}\t{}", store.string(store.packages.at(id).cpv),
                                        dep_kinds.at(dependency.kind), render(store, dependency)));
        }
    }
    sorted_unique(lines);
    return lines;
}

std::vector<std::uint32_t> neighborhood(const Graph& graph, std::span<const std::uint32_t> roots,
                                        std::uint32_t depth, Direction direction) {
    std::set<std::uint32_t> seen(roots.begin(), roots.end());
    std::vector<std::uint32_t> frontier(seen.begin(), seen.end());
    for (std::uint32_t distance = 0; distance < depth && !frontier.empty(); ++distance) {
        std::vector<std::uint32_t> next;
        for (const auto id : frontier) {
            if (direction != Direction::forward) {
                for (const auto& edge : graph.rdeps(id)) {
                    if (seen.insert(edge.parent).second) {
                        next.push_back(edge.parent);
                    }
                }
            }
            if (direction != Direction::reverse) {
                for (const auto& edge : graph.deps(id)) {
                    if (seen.insert(edge.child).second) {
                        next.push_back(edge.child);
                    }
                }
            }
        }
        frontier = std::move(next);
    }
    return {seen.begin(), seen.end()};
}

void write_dot(std::ostream& out, const Store& store, const Graph& graph,
               std::span<const std::uint32_t> packages, std::span<const std::uint32_t> roots) {
    const std::set<std::uint32_t> members(packages.begin(), packages.end());
    const std::set<std::uint32_t> filled(roots.begin(), roots.end());
    out << "digraph \"egraph\" {\n"
           "\trankdir=LR;\n"
           "\tnode [shape=box, fontname=\"monospace\", fontsize=10];\n"
           "\tedge [fontname=\"monospace\", fontsize=8];\n";
    for (const auto id : members) {
        out << '\t' << dot_id(store.string(store.packages.at(id).cpv))
            << (filled.contains(id) ? " [style=filled, fillcolor=\"#ffd966\"]" : "") << ";\n";
    }
    // One arrow per package pair, labelled with every kind that joins them; dashed when every
    // edge is an alternative inside a || group.
    for (const auto parent : members) {
        const auto edges = graph.deps(parent);
        // Sorted by child, so each child's edges are one run.
        for (std::size_t i = 0; i < edges.size();) {
            const auto child = element(edges, i).child;
            std::set<std::uint32_t> kinds;
            bool all_choices = true;
            for (; i < edges.size() && element(edges, i).child == child; ++i) {
                kinds.insert(element(edges, i).kind);
                all_choices = all_choices && element(edges, i).choice;
            }
            if (!members.contains(child)) {
                continue;
            }
            std::string label;
            for (const auto kind : kinds) {
                label += (label.empty() ? "" : ",");
                label += dep_kinds.at(kind);
            }
            out << '\t' << dot_id(store.string(store.packages.at(parent).cpv)) << " -> "
                << dot_id(store.string(store.packages.at(child).cpv)) << " [label=" << dot_id(label)
                << (all_choices ? ", style=dashed" : "") << "];\n";
        }
    }
    out << "}\n";
}

void write_stats(std::ostream& out, const Store& store, const Graph& graph,
                 const std::filesystem::path& path) {
    std::set<std::string_view> sonames;
    for (const auto& pkg : store.packages) {
        for (const auto& provided : store.pairs_in(pkg.provided)) {
            sonames.insert(store.string(provided.second));
        }
        for (const auto& required : store.required_in(pkg.required)) {
            sonames.insert(store.string(required.soname));
        }
    }
    const std::chrono::sys_time<std::chrono::seconds> built{
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::nanoseconds{store.meta.build_time_ns})};
    out << std::format("store: {}\neroot: {}\nbuilt: {:%FT%TZ}\n", path.string(), store.meta.eroot,
                       built)
        << std::format("packages: {}\ndependency nodes: {}\nedges: {}\nunsatisfied: {}\n",
                       store.packages.size(), store.nodes.size(), graph.forward.size(),
                       broken(store).size())
        << std::format("sonames: {}\nroot atoms: {}\ninputs: {}\n", sonames.size(),
                       store.roots.size(), store.inputs.size());
}

} // namespace egraph
