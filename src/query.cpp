#include "query.hpp"

#include "plan.hpp"
#include "remedy.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
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

std::string argument_text(const Argument& argument) {
    return argument.set.empty() ? argument.atom
                                : std::format("@{} {}", argument.set, argument.atom);
}

namespace {

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

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// The run of digits, or of anything else, that text starts with.
std::string_view run(std::string_view text, bool digits) {
    const auto end = std::ranges::find_if(text, [digits](char c) { return is_digit(c) != digits; });
    return text.substr(0, static_cast<std::size_t>(end - text.begin()));
}

// Digit runs by value, however long.
int compare_numbers(std::string_view a, std::string_view b) {
    a.remove_prefix(std::min(a.find_first_not_of('0'), a.size()));
    b.remove_prefix(std::min(b.find_first_not_of('0'), b.size()));
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return a.compare(b);
}

} // namespace

bool alnum_less(std::string_view a, std::string_view b) {
    // The key alternates text and digit runs, starting with a text run that may be empty.
    const auto whole_a = a;
    const auto whole_b = b;
    for (bool digits = false;; digits = !digits) {
        if (a.empty() || b.empty()) {
            if (!a.empty() || !b.empty()) {
                return a.empty();
            }
            break;
        }
        const auto run_a = run(a, digits);
        const auto run_b = run(b, digits);
        const int order = digits ? compare_numbers(run_a, run_b) : run_a.compare(run_b);
        if (order != 0) {
            return order < 0;
        }
        a.remove_prefix(run_a.size());
        b.remove_prefix(run_b.size());
    }
    return whole_a < whole_b;
}

std::string use_display(const Evaluated& evaluated, const Candidate& candidate) {
    const auto strings = [&evaluated](Range range) {
        std::vector<std::string_view> found;
        for (const auto id : evaluated.ids_in(range)) {
            found.push_back(evaluated.string(id));
        }
        return found;
    };
    const auto use = strings(candidate.use);
    const auto forced = strings(candidate.forced);
    const auto expand = strings(evaluated.use_expand);
    const auto hidden = strings(evaluated.use_expand_hidden);
    struct Flag {
        std::string_view name;
        bool enabled = false;
        bool forced = false;
    };
    // Groups by variable, "" for USE.
    std::map<std::string_view, std::vector<Flag>> groups;
    for (const auto flag : strings(candidate.iuse)) {
        std::string_view group;
        auto name = flag;
        for (const auto variable : expand) {
            if (flag.size() > variable.size() + 1 && flag.starts_with(variable) &&
                flag.at(variable.size()) == '_') {
                group = variable;
                name = flag.substr(variable.size() + 1);
                break;
            }
        }
        groups[group].push_back({.name = name,
                                 .enabled = std::ranges::binary_search(use, flag),
                                 .forced = std::ranges::binary_search(forced, flag)});
    }
    std::string shown;
    for (auto& [group, flags] : groups) {
        if (std::ranges::binary_search(hidden, group)) {
            continue;
        }
        std::ranges::sort(flags, [](const Flag& a, const Flag& b) {
            if (a.enabled != b.enabled) {
                return a.enabled;
            }
            return alnum_less(a.name, b.name);
        });
        std::string variable = group.empty() ? "USE" : std::string{group};
        std::ranges::transform(variable, variable.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
        shown += std::format("{}{}=\"", shown.empty() ? "" : " ", variable);
        for (std::size_t i = 0; i < flags.size(); ++i) {
            const auto& flag = flags.at(i);
            const auto text = std::format("{}{}", flag.enabled ? "" : "-", flag.name);
            shown += std::format("{}{}", i == 0 ? "" : " ",
                                 flag.forced ? std::format("({})", text) : text);
        }
        shown += '"';
    }
    return shown;
}

namespace {

std::optional<Version> version_of(std::string_view cp, std::string_view cpv) {
    return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
}

} // namespace

std::vector<std::uint32_t> fallbacks(const Evaluated& evaluated, std::uint32_t package,
                                     const PendingUpdate& wanted) {
    if (wanted.kind == UpdateKind::rebuild) {
        return {};
    }
    const auto& target = evaluated.candidates.at(wanted.target);
    const auto cp = evaluated.string(target.cp);
    const auto installed = version_of(cp, evaluated.string(evaluated.packages.at(package).cpv));
    struct Fallback {
        std::uint32_t index = 0;
        Version version;
    };
    std::vector<Fallback> found;
    for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
        const auto& candidate = evaluated.candidates.at(i);
        if (i == wanted.target || candidate.cp != target.cp || candidate.slot != target.slot ||
            !candidate.visible()) {
            continue;
        }
        auto version = version_of(cp, evaluated.string(candidate.cpv));
        if (!version || !installed || vercmp(*version, *installed) == 0 ||
            (wanted.kind == UpdateKind::upgrade && vercmp(*version, *installed) < 0)) {
            continue;
        }
        found.push_back({.index = i, .version = std::move(*version)});
    }
    std::ranges::stable_sort(found, [&](const Fallback& a, const Fallback& b) {
        if (const int order = vercmp(a.version, b.version); order != 0) {
            return order > 0;
        }
        return evaluated.candidates.at(a.index).repo == target.repo &&
               evaluated.candidates.at(b.index).repo != target.repo;
    });
    std::vector<std::uint32_t> indices;
    indices.reserve(found.size());
    for (const auto& fallback : found) {
        indices.push_back(fallback.index);
    }
    return indices;
}

std::vector<std::string> update_lines(const Store& store, const Evaluated& evaluated,
                                      UseRebuilds rebuilds, bool held, bool table,
                                      const Targets& targets,
                                      const std::optional<RemedyInputs>& remedies) {
    return update_lines(store, evaluated, plan_updates(store, evaluated, rebuilds, targets),
                        rebuilds, held, table, targets, remedies);
}

std::vector<std::string> update_lines(const Store& store, const Evaluated& evaluated,
                                      const Plan& plan, UseRebuilds rebuilds, bool held, bool table,
                                      const Targets& targets,
                                      const std::optional<RemedyInputs>& remedies) {
    const auto target_fields = [&evaluated](std::uint32_t target) {
        const auto& candidate = evaluated.candidates.at(target);
        return std::format("{}\t{}", evaluated.string(candidate.cpv),
                           evaluated.string(candidate.repo));
    };
    const auto member = [&](const Member& found) {
        return found.candidate ? evaluated.string(evaluated.candidates.at(found.index).cpv)
                               : store.string(store.packages.at(found.index).cpv);
    };
    std::vector<std::string> merge_lines;
    for (const auto& merge : plan.merges) {
        if (merge.replaces) {
            auto line =
                std::format("{}\t{}\t{}", store.string(store.packages.at(*merge.replaces).cpv),
                            kind_name(merge.kind), target_fields(merge.candidate));
            if (!merge.flags.empty() || merge.rebuilt_for) {
                line += std::format("\t{}", merge.flags);
            }
            if (const auto& why = merge.rebuilt_for) {
                line += std::format("\t{} {}", member(why->member), why->atom);
            }
            merge_lines.push_back(std::move(line));
        } else {
            const auto cpv = evaluated.string(evaluated.candidates.at(merge.candidate).cpv);
            auto line = std::format("{}\tnew\t{}", cpv, target_fields(merge.candidate));
            std::string why;
            if (const auto& by = merge.pulled_by) {
                why = std::format("{} {}", member(by->member), by->atom);
            } else if (merge.named_by) {
                why = argument_text(*merge.named_by);
            }
            const auto use = use_display(evaluated, evaluated.candidates.at(merge.candidate));
            if (!use.empty() || !why.empty()) {
                line += std::format("\t{}", use);
            }
            if (!why.empty()) {
                line += std::format("\t{}", why);
            }
            merge_lines.push_back(std::move(line));
        }
    }
    const auto package = [&store](std::uint32_t id) {
        return store.string(store.packages.at(id).cpv);
    };
    // Per package, its held line and any remedies.
    std::vector<std::vector<std::string>> held_lines(store.packages.size());
    if (held) {
        const auto found = remedies ? egraph::remedies(store, evaluated, remedies->graph, plan,
                                                       rebuilds, targets, remedies->rescope)
                                    : std::vector<Remedy>{};
        for (std::size_t index = 0; index < plan.held.size(); ++index) {
            const auto& back = plan.held.at(index);
            const auto cpv = package(back.package);
            auto line = std::format("{}\theld\t{}\t{}", cpv, target_fields(back.wanted.target),
                                    back.wanted.flags);
            // Reasons are sorted by member, then atom.
            for (std::size_t i = 0; i < back.reasons.size(); ++i) {
                const auto& reason = back.reasons.at(i);
                if (i == 0 || back.reasons.at(i - 1).member != reason.member) {
                    line += std::format("\t{}", member(reason.member));
                }
                line += std::format(" {}", reason.atom);
            }
            auto& group = held_lines.at(back.package);
            group.push_back(std::move(line));
            if (index < found.size()) {
                const auto& remedy = found.at(index);
                for (const auto& holder : remedy.holders) {
                    line = std::format("{}\tholder\t{}\t", cpv, package(holder.package));
                    for (std::size_t i = 0; i < holder.dependents.size(); ++i) {
                        line += std::format("{}{}", i == 0 ? "" : " ",
                                            package(holder.dependents.at(i)));
                    }
                    for (const auto root : holder.roots) {
                        const auto& atom = store.roots.at(root);
                        line += std::format("\t@{} {}", store.string(atom.set),
                                            store.string(atom.atom));
                    }
                    group.push_back(std::move(line));
                }
                if (remedy.removable) {
                    line = std::format("{}\tremove\t", cpv);
                    for (std::size_t i = 0; i < remedy.frees.size(); ++i) {
                        line += std::format("{}{}", i == 0 ? "" : " ",
                                            package(plan.held.at(remedy.frees.at(i)).package));
                    }
                    group.push_back(std::move(line));
                }
                if (remedy.nodeps) {
                    group.push_back(std::format("{}\tnodeps", cpv));
                }
            }
        }
    }
    // Uninstalls, then blocks, after everything else.
    std::vector<std::string> blocker_rows;
    blocker_rows.reserve(plan.uninstalls.size() + plan.blocks.size());
    const auto block_fields = [&](const Block& block) {
        return std::format("{}\t{}\t{}", member(block.holder), block.atom, member(block.blocked));
    };
    for (const auto& each : plan.uninstalls) {
        blocker_rows.push_back(
            std::format("{}\tuninstall\t{}", package(each.package), block_fields(each.why)));
    }
    for (const auto& each : plan.blocks) {
        blocker_rows.push_back(std::format("{}\tblocks\t{}\t{}", member(each.holder), each.atom,
                                           member(each.blocked)));
    }
    std::vector<std::string> lines;
    if (table) {
        std::vector<std::size_t> place(plan.merges.size());
        for (std::size_t i = 0; i < plan.order.size(); ++i) {
            place.at(plan.order.at(i)) = i + 1;
        }
        for (std::size_t i = 0; i < plan.order.size(); ++i) {
            const auto merge = plan.order.at(i);
            std::string waits;
            for (const auto wait : plan.merges.at(merge).waits) {
                waits += std::format("{}{}", waits.empty() ? "" : " ", place.at(wait));
            }
            lines.push_back(std::format("{}\t{}\t{}", i + 1, waits, merge_lines.at(merge)));
        }
        for (const auto& group : held_lines) {
            for (const auto& line : group) {
                lines.push_back(std::format("\t\t{}", line));
            }
        }
        for (const auto& line : blocker_rows) {
            lines.push_back(std::format("\t\t{}", line));
        }
        return lines;
    }
    std::vector<std::optional<std::string>> replaced(store.packages.size());
    std::vector<std::string> added;
    for (std::size_t i = 0; i < plan.merges.size(); ++i) {
        if (const auto id = plan.merges.at(i).replaces) {
            replaced.at(*id) = std::move(merge_lines.at(i));
        } else {
            added.push_back(std::move(merge_lines.at(i)));
        }
    }
    for (std::size_t id = 0; id < store.packages.size(); ++id) {
        if (auto& line = replaced.at(id)) {
            lines.push_back(std::move(*line));
        }
        std::ranges::move(held_lines.at(id), std::back_inserter(lines));
    }
    std::ranges::move(added, std::back_inserter(lines));
    std::ranges::move(blocker_rows, std::back_inserter(lines));
    return lines;
}

std::vector<std::string> update_tree_lines(const Store& store, const Evaluated& evaluated,
                                           const Kept& kept, UseRebuilds rebuilds,
                                           const Targets& targets) {
    return update_tree_lines(store, evaluated, kept,
                             plan_updates(store, evaluated, rebuilds, targets));
}

std::vector<std::string> update_tree_lines(const Store& store, const Evaluated& evaluated,
                                           const Kept& kept, const Plan& plan) {
    std::map<std::uint32_t, std::uint32_t> by_candidate;
    for (std::uint32_t i = 0; i < plan.merges.size(); ++i) {
        by_candidate.emplace(plan.merges.at(i).candidate, i);
    }
    // "@set<TAB>cpv<TAB>..." down to an installed package, or an empty set and its cpv.
    const auto installed_chain = [&](std::uint32_t id) {
        const auto path = why(kept, id);
        if (!path) {
            return std::format("\t{}", store.string(store.packages.at(id).cpv));
        }
        auto chain = std::format("@{}\t{}", store.string(store.roots.at(path->root.root).set),
                                 store.string(store.packages.at(path->root.child).cpv));
        for (const auto& edge : path->edges) {
            chain += std::format("\t{}", store.string(store.packages.at(edge.child).cpv));
        }
        return chain;
    };
    std::vector<std::optional<std::string>> chains(plan.merges.size());
    for (std::size_t i = 0; i < plan.merges.size(); ++i) {
        if (const auto id = plan.merges.at(i).replaces) {
            chains.at(i) = installed_chain(*id);
        }
    }
    // New packages hang from what pulled them in, which may itself be new.
    for (bool grew = true; grew;) {
        grew = false;
        for (std::size_t i = 0; i < plan.merges.size(); ++i) {
            const auto& merge = plan.merges.at(i);
            if (chains.at(i)) {
                continue;
            }
            const auto cpv = evaluated.string(evaluated.candidates.at(merge.candidate).cpv);
            if (merge.named_by) {
                const auto& set = merge.named_by->set;
                chains.at(i) = std::format("{}{}\t{}", set.empty() ? "" : "@", set, cpv);
                grew = true;
                continue;
            }
            if (!merge.pulled_by) {
                continue;
            }
            const auto& by = merge.pulled_by->member;
            if (!by.candidate) {
                chains.at(i) = std::format("{}\t{}", installed_chain(by.index), cpv);
            } else if (const auto puller = by_candidate.find(by.index);
                       puller != by_candidate.end()) {
                const auto& above = chains.at(puller->second);
                if (!above) {
                    continue;
                }
                chains.at(i) = std::format("{}\t{}", *above, cpv);
            } else {
                continue;
            }
            grew = true;
        }
    }
    for (std::size_t i = 0; i < plan.merges.size(); ++i) {
        if (!chains.at(i)) {
            chains.at(i) = std::format(
                "\t{}", evaluated.string(evaluated.candidates.at(plan.merges.at(i).candidate).cpv));
        }
    }
    std::vector<std::string> lines;
    lines.reserve(plan.order.size());
    for (std::size_t place = 0; place < plan.order.size(); ++place) {
        lines.push_back(
            std::format("{}\t{}", place + 1, chains.at(plan.order.at(place)).value_or("")));
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
