#include "query.hpp"

#include "atom.hpp"
#include "version.hpp"

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

std::vector<std::string> possible_lines(const Evaluated& evaluated,
                                        std::span<const std::uint32_t> packages, bool reverse) {
    const auto cpv = [&evaluated](std::uint32_t id) {
        return evaluated.string(evaluated.packages.at(id).cpv);
    };
    std::vector<std::string> lines;
    const auto add = [&](std::uint32_t parent) {
        for (const auto& entry : evaluated.possible_in(evaluated.packages.at(parent).possible)) {
            std::string flags;
            for (const auto id : evaluated.ids_in(entry.flags)) {
                flags += flags.empty() ? "" : " ";
                flags += evaluated.string(id);
            }
            for (const auto child : evaluated.ids_in(entry.matches)) {
                if (reverse && !std::ranges::binary_search(packages, child)) {
                    continue;
                }
                lines.push_back(std::format("{}\t{}\t{}\t{}{}\tuse={}", cpv(parent),
                                            dep_kinds.at(entry.kind), evaluated.string(entry.atom),
                                            cpv(child), entry.choice ? "\tany-of" : "", flags));
            }
        }
    };
    if (reverse) {
        for (std::uint32_t parent = 0; parent < evaluated.packages.size(); ++parent) {
            add(parent);
        }
    } else {
        for (const auto parent : packages) {
            add(parent);
        }
    }
    sorted_unique(lines);
    return lines;
}

namespace {

UpdateKind update_kind(std::string_view cp, std::string_view from, std::string_view to) {
    const auto version = [&cp](std::string_view cpv) {
        return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
    };
    const auto old = version(from);
    const auto target = version(to);
    const int order = old && target ? vercmp(*target, *old) : 0;
    return order > 0   ? UpdateKind::upgrade
           : order < 0 ? UpdateKind::downgrade
                       : UpdateKind::rebuild;
}

constexpr std::string_view kind_name(UpdateKind kind) {
    switch (kind) {
    case UpdateKind::upgrade:
        return "upgrade";
    case UpdateKind::downgrade:
        return "downgrade";
    case UpdateKind::rebuild:
        return "rebuild";
    }
    return "rebuild";
}

// A --changed-use flag: its state changed, which emerge marks with a *.
bool state_changed(std::string_view flag) {
    return flag.ends_with('*') || flag.ends_with("*)");
}

} // namespace

std::optional<PendingUpdate> pending_update(const Evaluated& evaluated, std::uint32_t package,
                                            UseRebuilds rebuilds) {
    const auto& pkg = evaluated.packages.at(package);
    if (!pkg.target) {
        return std::nullopt;
    }
    const auto& target = evaluated.candidates.at(*pkg.target);
    if (pkg.rebuild.count == 0) {
        return PendingUpdate{.kind =
                                 update_kind(evaluated.string(target.cp), evaluated.string(pkg.cpv),
                                             evaluated.string(target.cpv)),
                             .target = *pkg.target,
                             .flags = {}};
    }
    std::string flags;
    for (const auto id : evaluated.ids_in(pkg.rebuild)) {
        const auto flag = evaluated.string(id);
        if (rebuilds == UseRebuilds::all ||
            (rebuilds == UseRebuilds::changed && state_changed(flag))) {
            flags += flags.empty() ? "" : " ";
            flags += flag;
        }
    }
    if (flags.empty()) {
        return std::nullopt;
    }
    return PendingUpdate{.kind = UpdateKind::rebuild, .target = *pkg.target, .flags = flags};
}

namespace {

std::optional<Version> version_of(std::string_view cp, std::string_view cpv) {
    return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
}

// The atoms of parents' dependencies that would go unsatisfied were package replaced by the
// candidate, sorted.
std::vector<Holder> holders_of(const Store& store, const Evaluated& evaluated,
                               std::span<const std::uint32_t> parents, std::uint32_t package,
                               const Candidate& candidate) {
    std::vector<Holder> holders;
    for (const auto parent : parents) {
        for (const auto range : store.packages.at(parent).deps) {
            const auto nodes = store.nodes_in(range);
            const auto matched = [&](std::size_t i) {
                return std::ranges::contains(store.ids_in(element(nodes, i).matches), package);
            };
            const auto accepts = [&](std::size_t i) {
                auto atom = parse_atom(store.string(element(nodes, i).atom));
                if (!atom) {
                    return true;
                }
                if (atom->slot_operator) {
                    atom->sub_slot.reset();
                }
                return matches(store, evaluated, candidate, *atom);
            };
            const auto after = satisfied(nodes, [&](std::size_t i) {
                const auto ids = store.ids_in(element(nodes, i).matches);
                return std::ranges::any_of(ids, [package](auto id) { return id != package; }) ||
                       (matched(i) && accepts(i));
            });
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const auto& node = element(nodes, i);
                if (node.type != NodeType::atom || !matched(i) || accepts(i)) {
                    continue;
                }
                auto top = i;
                while (element(nodes, top).parent != no_parent) {
                    top = element(nodes, top).parent;
                }
                if (!after.at(top)) {
                    holders.push_back({.parent = parent, .atom = node.atom});
                }
            }
        }
    }
    std::ranges::sort(holders);
    const auto [first, last] = std::ranges::unique(holders);
    holders.erase(first, last);
    return holders;
}

} // namespace

WeighedUpdate weigh_update(const Store& store, const Graph& graph, const Evaluated& evaluated,
                           std::uint32_t package, UseRebuilds rebuilds,
                           const std::vector<bool>& scope) {
    auto wanted = pending_update(evaluated, package, rebuilds);
    if (!wanted) {
        return {};
    }
    std::vector<std::uint32_t> parents;
    for (const auto& edge : graph.rdeps(package)) {
        if (edge.parent != package && (scope.empty() || scope.at(edge.parent))) {
            parents.push_back(edge.parent);
        }
    }
    std::ranges::sort(parents);
    const auto [first, last] = std::ranges::unique(parents);
    parents.erase(first, last);

    const auto& target = evaluated.candidates.at(wanted->target);
    auto holders = holders_of(store, evaluated, parents, package, target);
    if (holders.empty()) {
        return {.update = std::move(wanted), .held = {}, .holders = {}};
    }
    WeighedUpdate weighed{.update = {}, .held = wanted, .holders = std::move(holders)};
    // A rebuild for USE has no other version to fall back to.
    if (wanted->kind == UpdateKind::rebuild) {
        return weighed;
    }
    const auto cp = evaluated.string(target.cp);
    const auto cpv = evaluated.string(evaluated.packages.at(package).cpv);
    const auto installed = version_of(cp, cpv);
    // Other visible versions in the slot, newer than the installed one for an upgrade, best first
    // and of one version the target's repository first.
    struct Fallback {
        std::uint32_t index = 0;
        Version version;
    };
    std::vector<Fallback> fallbacks;
    for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
        const auto& candidate = evaluated.candidates.at(i);
        if (i == wanted->target || candidate.cp != target.cp || candidate.slot != target.slot ||
            !candidate.visible()) {
            continue;
        }
        auto version = version_of(cp, evaluated.string(candidate.cpv));
        if (!version || !installed || vercmp(*version, *installed) == 0 ||
            (wanted->kind == UpdateKind::upgrade && vercmp(*version, *installed) < 0)) {
            continue;
        }
        fallbacks.push_back({.index = i, .version = std::move(*version)});
    }
    std::ranges::stable_sort(fallbacks, [&](const Fallback& a, const Fallback& b) {
        if (const int order = vercmp(a.version, b.version); order != 0) {
            return order > 0;
        }
        return evaluated.candidates.at(a.index).repo == target.repo &&
               evaluated.candidates.at(b.index).repo != target.repo;
    });
    for (const auto& fallback : fallbacks) {
        const auto& candidate = evaluated.candidates.at(fallback.index);
        if (holders_of(store, evaluated, parents, package, candidate).empty()) {
            weighed.update =
                PendingUpdate{.kind = update_kind(cp, cpv, evaluated.string(candidate.cpv)),
                              .target = fallback.index,
                              .flags = {}};
            break;
        }
    }
    return weighed;
}

std::vector<std::string> update_lines(const Store& store, const Graph& graph,
                                      const Evaluated& evaluated, UseRebuilds rebuilds, bool held,
                                      const std::vector<bool>& scope) {
    std::vector<std::string> lines;
    const auto target_fields = [&evaluated](std::uint32_t target) {
        const auto& candidate = evaluated.candidates.at(target);
        return std::format("{}\t{}", evaluated.string(candidate.cpv),
                           evaluated.string(candidate.repo));
    };
    for (std::uint32_t id = 0; id < evaluated.packages.size(); ++id) {
        const auto weighed = weigh_update(store, graph, evaluated, id, rebuilds, scope);
        const auto cpv = evaluated.string(evaluated.packages.at(id).cpv);
        if (const auto& update = weighed.update) {
            auto line = std::format("{}\t{}\t{}", cpv, kind_name(update->kind),
                                    target_fields(update->target));
            if (!update->flags.empty()) {
                line += std::format("\t{}", update->flags);
            }
            lines.push_back(std::move(line));
        }
        if (held && weighed.held) {
            for (const auto& holder : weighed.holders) {
                lines.push_back(std::format(
                    "{}\theld\t{}\t{}\t{}", cpv, target_fields(weighed.held->target),
                    store.string(store.packages.at(holder.parent).cpv), store.string(holder.atom)));
            }
        }
    }
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

std::vector<std::uint32_t> installed_instead(const Store& store, const Unsatisfied& dependency) {
    const auto& pkg = store.packages.at(dependency.package);
    const auto nodes = store.nodes_in(pkg.deps.at(dependency.kind));
    // The node and every node below it; children follow their parents.
    const auto within = [&](std::uint32_t index) {
        while (index != no_parent && index != dependency.node) {
            index = element(nodes, index).parent;
        }
        return index == dependency.node;
    };
    std::vector<std::string> cps;
    for (auto i = dependency.node; i < nodes.size(); ++i) {
        const auto& node = element(nodes, i);
        if (node.type != NodeType::atom || !within(i)) {
            continue;
        }
        if (const auto atom = parse_atom(store.string(node.atom))) {
            cps.push_back(atom->cp);
        }
    }
    std::vector<std::uint32_t> found;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        if (std::ranges::contains(cps, store.string(store.packages.at(id).cp))) {
            found.push_back(id);
        }
    }
    return found;
}

bool replaced(const Unsatisfied& dependency, std::span<const std::uint32_t> instead) {
    return is_build_kind(dependency.kind) && !instead.empty();
}

BrokenRecords broken_records(const Store& store) {
    BrokenRecords records;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        for (const auto& dependency : unsatisfied(store, id)) {
            const auto instead = installed_instead(store, dependency);
            auto record = std::format("{}\t{}\t{}", store.string(store.packages.at(id).cpv),
                                      dep_kinds.at(dependency.kind), render(store, dependency));
            for (std::size_t i = 0; i < instead.size(); ++i) {
                record += i == 0 ? '\t' : ' ';
                record += store.string(store.packages.at(instead.at(i)).cpv);
            }
            (replaced(dependency, instead) ? records.replaced : records.broken)
                .push_back(std::move(record));
        }
    }
    sorted_unique(records.broken);
    sorted_unique(records.replaced);
    return records;
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
