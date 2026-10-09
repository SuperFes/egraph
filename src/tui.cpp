#include "tui.hpp"

#include "build_info.hpp"
#include "request.hpp"

#include <cmath>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <span>
#include <tuple>

namespace egraph::tui {

namespace {

// count code points of text, after the first skip.
std::string code_points(std::string_view text, std::size_t skip, std::size_t count) {
    return clip(text, skip + count).substr(clip(text, skip).size());
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> found;
    for (const auto line : std::views::split(text, '\n')) {
        found.emplace_back(std::string_view{line});
    }
    return found;
}

std::string folded(std::string_view text) {
    std::string out{text};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void append_utf8(std::string& out, char32_t code) {
    const auto byte = [](std::uint32_t value) { return static_cast<char>(value); };
    const auto c = static_cast<std::uint32_t>(code);
    if (c < 0x80U) {
        out += byte(c);
    } else if (c < 0x800U) {
        out += byte(0xC0U | (c >> 6U));
        out += byte(0x80U | (c & 0x3FU));
    } else if (c < 0x10000U) {
        out += byte(0xE0U | (c >> 12U));
        out += byte(0x80U | ((c >> 6U) & 0x3FU));
        out += byte(0x80U | (c & 0x3FU));
    } else {
        out += byte(0xF0U | (c >> 18U));
        out += byte(0x80U | ((c >> 12U) & 0x3FU));
        out += byte(0x80U | ((c >> 6U) & 0x3FU));
        out += byte(0x80U | (c & 0x3FU));
    }
}

void pop_code_point(std::string& text) {
    while (!text.empty()) {
        const auto last = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((last & 0xC0U) != 0x80U) {
            return;
        }
    }
}

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

// Index into kind_shorthands for an index into dep_kinds.
std::size_t shorthand_of(std::uint32_t kind) {
    const auto name = dep_kinds.at(kind);
    const auto found = std::ranges::find(kind_shorthands, name, &KindShorthand::name);
    return static_cast<std::size_t>(found - kind_shorthands.begin());
}

bool is(const Key& key, char32_t code) {
    return key.kind == KeyKind::character && key.code == code;
}

void keep_visible(Cursor& cursor, std::size_t height) {
    if (cursor.at < cursor.top) {
        cursor.top = cursor.at;
    } else if (cursor.at >= cursor.top + height) {
        cursor.top = cursor.at - height + 1;
    }
}

// Moves over a plain list of count entries.
void move(Cursor& cursor, std::size_t count, const Key& key, std::size_t height) {
    if (count == 0) {
        cursor = {};
        return;
    }
    auto at = cursor.at;
    if (key.kind == KeyKind::up || is(key, U'k')) {
        at = at == 0 ? 0 : at - 1;
    } else if (key.kind == KeyKind::down || is(key, U'j')) {
        at = at + 1;
    } else if (key.kind == KeyKind::page_up) {
        at = at > height ? at - height : 0;
    } else if (key.kind == KeyKind::page_down) {
        at = at + height;
    } else if (key.kind == KeyKind::home || is(key, U'g')) {
        at = 0;
    } else if (key.kind == KeyKind::end || is(key, U'G')) {
        at = count - 1;
    }
    cursor.at = std::min(at, count - 1);
    keep_visible(cursor, height);
}

bool is_move(const Key& key) {
    switch (key.kind) {
    case KeyKind::up:
    case KeyKind::down:
    case KeyKind::page_up:
    case KeyKind::page_down:
    case KeyKind::home:
    case KeyKind::end:
        return true;
    default:
        return is(key, U'j') || is(key, U'k') || is(key, U'g') || is(key, U'G');
    }
}

bool selectable(const Row& row) {
    return row.type == RowType::link || row.type == RowType::path;
}

// Moves over rows, landing only on those at stops (ascending); rows above the first stay in view.
void move_among(Cursor& cursor, const std::vector<std::size_t>& stops, const Key& key,
                std::size_t height) {
    if (stops.empty()) {
        return;
    }
    const auto current = std::ranges::lower_bound(stops, cursor.at);
    Cursor among{.at = static_cast<std::size_t>(current - stops.begin()), .top = 0};
    among.at = std::min(among.at, stops.size() - 1);
    move(among, stops.size(), key, height);
    cursor.at = stops.at(among.at);
    keep_visible(cursor, height);
    if (among.at == 0) {
        cursor.top = 0;
    }
}

// Moves over a page, landing only on packages, versions and flags.
void move_on_page(App::Page& page, const Key& key, std::size_t height) {
    std::vector<std::size_t> stops;
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        if (const auto type = page.rows.at(i).type;
            selectable(page.rows.at(i)) || type == RowType::version || type == RowType::flag) {
            stops.push_back(i);
        }
    }
    move_among(page.cursor, stops, key, height);
}

std::string_view member_cpv(const Store& store, const Evaluated& evaluated, const Member& member) {
    return member.candidate ? evaluated.string(evaluated.candidates.at(member.index).cpv)
                            : store.string(store.packages.at(member.index).cpv);
}

Row text_row(RowType type, std::string text) {
    Row row;
    row.type = type;
    row.text = std::move(text);
    return row;
}

Row link_row(const Link& link, std::size_t depth, bool reverse) {
    Row row;
    row.type = RowType::link;
    row.link = link;
    row.depth = depth;
    row.reverse = reverse;
    return row;
}

// Why depclean keeps package: the root, then each package down the chain to it.
std::vector<Row> kept_rows(const Store& store, const Kept& kept, std::uint32_t package,
                           bool build_deps) {
    std::vector<Row> rows{
        text_row(RowType::heading, build_deps ? "Kept by" : "Kept by, at run time")};
    const auto path = why(kept, package);
    if (!path) {
        rows.push_back(text_row(RowType::alert, store.roots.empty()
                                                    ? "nothing: @world is empty, which depclean "
                                                      "refuses to work with"
                                                    : "nothing: depclean would remove it"));
    } else {
        const auto& root = store.roots.at(path->root.root);
        auto root_row = text_row(RowType::root, std::format("@{}", store.string(root.set)));
        root_row.link.atom = root.atom;
        rows.push_back(std::move(root_row));
        auto first = link_row({.package = path->root.child, .atom = root.atom}, 1, false);
        first.type = RowType::path;
        rows.push_back(std::move(first));
        for (const auto& edge : path->edges) {
            Link link{.package = edge.child, .atom = edge.atom, .choice = edge.choice};
            if (const auto index = shorthand_of(edge.kind); index < kind_shorthands.size()) {
                link.kinds.at(index) = true;
            }
            auto row = link_row(link, rows.back().depth + 1, false);
            row.type = RowType::path;
            rows.push_back(std::move(row));
        }
    }
    rows.push_back(text_row(RowType::note, ""));
    return rows;
}

// What package needs that nothing installed satisfies, then the build-time dependencies since
// replaced; one row per dependency with its kinds and what is installed in its place.
std::vector<Row> unsatisfied_rows(const Store& store, std::uint32_t package, bool build_deps) {
    std::vector<Row> missing;
    for (const auto& dependency : unsatisfied(store, package)) {
        if (!build_deps && is_build_kind(dependency.kind)) {
            continue;
        }
        auto instead = installed_instead(store, dependency);
        const auto type = replaced(dependency, instead) ? RowType::replaced : RowType::missing;
        auto text = render(store, dependency);
        auto row = std::ranges::find_if(
            missing, [&](const Row& other) { return other.type == type && other.text == text; });
        if (row == missing.end()) {
            missing.push_back(text_row(type, std::move(text)));
            row = missing.end() - 1;
            row->instead = std::move(instead);
        }
        if (const auto index = shorthand_of(dependency.kind); index < kind_shorthands.size()) {
            row->link.kinds.at(index) = true;
        }
    }
    std::vector<Row> rows;
    for (const auto type : {RowType::missing, RowType::replaced}) {
        const auto count = std::ranges::count(missing, type, &Row::type);
        if (count == 0) {
            continue;
        }
        rows.push_back(text_row(
            RowType::heading,
            std::format("{}  {}",
                        type == RowType::missing ? "Not installed" : "Built with, since replaced",
                        count)));
        std::ranges::copy_if(missing, std::back_inserter(rows),
                             [&](const Row& row) { return row.type == type; });
        rows.push_back(text_row(RowType::note, ""));
    }
    return rows;
}

// What emerge -uD would do to package, with the merge a slot-operator rebuild is for.
std::vector<Row> update_rows(const std::optional<PendingUpdate>& update, std::string rebuilt_for) {
    if (!update) {
        return {};
    }
    auto row = text_row(RowType::update, std::move(rebuilt_for));
    row.update = update;
    return {text_row(RowType::heading, "Update"), std::move(row), text_row(RowType::note, "")};
}

// The update the plan holds back, each holder as a link with the atoms that hold it and what
// keeps it, then the commands past it.
std::vector<Row> held_rows(const Store& store, const Graph& graph, const Evaluated& evaluated,
                           const Plan& plan, const Remedy& remedy) {
    const auto& back = plan.held.at(remedy.held);
    const auto dependents = links(store, graph, back.package, true);
    auto held = text_row(RowType::held, "");
    held.update = back.wanted;
    std::vector<Row> rows{text_row(RowType::heading, "Held back"), std::move(held)};
    const auto cpv = [&store](std::uint32_t id) { return store.string(store.packages.at(id).cpv); };
    // Reasons name a member by its cpv; a rebuild's shares its installed package's.
    const auto member = [&](const Member& found) {
        return found.candidate ? evaluated.string(evaluated.candidates.at(found.index).cpv)
                               : cpv(found.index);
    };
    std::vector<std::string_view> holders;
    std::vector<std::string_view> deselect;
    for (const auto& holder : remedy.holders) {
        std::string atoms;
        for (const auto& reason : back.reasons) {
            if (member(reason.member) == cpv(holder.package)) {
                atoms += std::format("{}{}", atoms.empty() ? "" : " ", reason.atom);
            }
        }
        auto link = link_row({.package = holder.package, .atom = 0, .choice = false}, 0, true);
        // The kinds its installed dependencies name the held package through.
        for (const auto& found : dependents) {
            if (found.package == holder.package) {
                for (std::size_t k = 0; k < found.kinds.size(); ++k) {
                    link.link.kinds.at(k) = link.link.kinds.at(k) || found.kinds.at(k);
                }
            }
        }
        link.text = std::move(atoms);
        rows.push_back(std::move(link));
        std::vector<std::string_view> needing;
        needing.reserve(holder.dependents.size());
        for (const auto dependent : holder.dependents) {
            needing.push_back(cpv(dependent));
        }
        std::vector<std::string> sets;
        for (const auto root : holder.roots) {
            const auto& atom = store.roots.at(root);
            sets.push_back(std::format("@{}", store.string(atom.set)));
            if (store.string(atom.set) == "selected" && store.string(atom.via).empty()) {
                deselect.push_back(store.string(atom.atom));
            }
        }
        const std::vector<std::string_view> set_views(sets.begin(), sets.end());
        rows.push_back(text_row(RowType::note, holder_note(needing, set_views)));
        holders.push_back(cpv(holder.package));
    }
    // The rest of what holds it: its own dependencies, or those of what it would pull in.
    for (const auto& reason : back.reasons) {
        const auto name = member(reason.member);
        if (std::ranges::find(holders, name) == holders.end()) {
            rows.push_back(text_row(RowType::note, std::format("{}  {}", name, reason.atom)));
        }
    }
    std::optional<std::vector<std::string_view>> frees;
    if (remedy.removable) {
        frees.emplace();
        for (const auto freed : remedy.frees) {
            frees->push_back(cpv(plan.held.at(freed).package));
        }
    }
    const auto lines = remedy_lines(
        holders, deselect, evaluated.string(evaluated.candidates.at(back.wanted.target).cpv), frees,
        remedy.nodeps);
    std::size_t width = 0;
    for (const auto& line : lines) {
        width = std::max(width, line.label.size() + 1);
    }
    for (auto line : lines) {
        line.label.resize(width, ' ');
        auto row = text_row(RowType::remedy, "");
        row.remedy = std::move(line);
        rows.push_back(std::move(row));
    }
    rows.push_back(text_row(RowType::note, ""));
    return rows;
}

// A possible dependency's toggles as the user would set them: "+flag" or "-flag".
std::string toggles(const Evaluated& evaluated, const Possible& entry) {
    std::string out;
    for (const auto id : evaluated.ids_in(entry.flags)) {
        const auto flag = evaluated.string(id);
        out +=
            std::format("{}{}{}", out.empty() ? "" : " ", flag.starts_with('-') ? "" : "+", flag);
    }
    return out;
}

// What package's ebuild would depend on with flags toggled (or, reverse, whose ebuilds would
// depend on package): one link per package, atom, choice and toggles with the kinds combined,
// sorted as links are, then forward what nothing installed satisfies.
// The USE of the package's own version as its ebuild stacks: its explicit IUSE, each flag with
// where it was last set, and the files package.env gives it; nothing without the ebuild.
std::vector<Row> use_rows(const Store& installed, const Evaluated& evaluated,
                          const Evaluated& untried, std::uint32_t package) {
    const auto own = evaluated.packages.at(package).own;
    if (!own) {
        return {};
    }
    const auto& candidate = evaluated.candidates.at(*own);
    const UseStacker stacker(installed, evaluated);
    std::vector<FlagState> states;
    for (auto& state : flag_states(evaluated, candidate, stacker.stack(candidate))) {
        if (state.explicit_iuse) {
            states.push_back(std::move(state));
        }
    }
    // Without the lines tried: the same candidates, as with_what_if keeps them.
    std::vector<std::string> before;
    if (&untried != &evaluated && *own < untried.candidates.size()) {
        const UseStacker untried_stacker(installed, untried);
        before = untried_stacker.stack(untried.candidates.at(*own)).use;
    }
    const auto env = stacker.env_files(candidate);
    if (states.empty() && env.empty()) {
        return {};
    }
    std::vector<Row> rows{text_row(RowType::note, ""),
                          text_row(RowType::heading, std::format("USE  {}", states.size()))};
    if (!env.empty()) {
        std::string files;
        for (const auto& file : env) {
            files += (files.empty() ? "" : " ") + file;
        }
        rows.push_back(text_row(RowType::note, "package.env  " + files));
    }
    std::size_t width = 0;
    for (const auto& state : states) {
        width =
            std::max(width, state.flag.size() + (state.enabled ? 0 : 1) + (state.fixed ? 2 : 0));
    }
    for (auto& state : states) {
        const auto& last = state.last;
        auto row = text_row(RowType::flag, use_place(last ? layer_name(last->layer) : "",
                                                     last ? step_place(evaluated, *last) : "",
                                                     last ? std::string_view{last->token} : ""));
        if (&untried != &evaluated) {
            if (const bool was = std::ranges::binary_search(before, state.flag);
                was != state.enabled) {
                row.was = was;
            }
        }
        row.flag = std::move(state);
        row.column = width + 2;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Row> possible_rows(const Store& store, const Evaluated& evaluated,
                               std::uint32_t package, bool reverse) {
    std::vector<Row> found;
    // other is the linked package; none for what nothing installed satisfies.
    const auto add = [&](const Possible& entry, std::optional<std::uint32_t> other) {
        auto flags = toggles(evaluated, entry);
        std::string atom{evaluated.string(entry.atom)};
        const auto type = other ? RowType::link : RowType::unmatched;
        const auto id = other.value_or(0);
        auto row = std::ranges::find_if(found, [&](const Row& candidate) {
            return candidate.type == type && candidate.link.package == id &&
                   candidate.text == atom && candidate.link.choice == entry.choice &&
                   candidate.flags == flags;
        });
        if (row == found.end()) {
            auto made = link_row({.package = id, .atom = 0, .choice = entry.choice}, 0, reverse);
            made.type = type;
            made.text = std::move(atom);
            made.flags = std::move(flags);
            found.push_back(std::move(made));
            row = found.end() - 1;
        }
        if (const auto index = shorthand_of(entry.kind); index < kind_shorthands.size()) {
            row->link.kinds.at(index) = true;
        }
    };
    if (reverse) {
        for (std::uint32_t parent = 0; parent < evaluated.packages.size(); ++parent) {
            for (const auto& entry :
                 evaluated.possible_in(evaluated.packages.at(parent).possible)) {
                if (std::ranges::contains(evaluated.ids_in(entry.matches), package)) {
                    add(entry, parent);
                }
            }
        }
    } else {
        for (const auto& entry : evaluated.possible_in(evaluated.packages.at(package).possible)) {
            const auto matches = evaluated.ids_in(entry.matches);
            if (matches.empty()) {
                add(entry, std::nullopt);
            }
            for (const auto child : matches) {
                add(entry, child);
            }
        }
    }
    const auto key = [&store](const Row& row) {
        return std::tuple{row.type == RowType::unmatched,
                          row.type == RowType::link
                              ? store.string(store.packages.at(row.link.package).cpv)
                              : std::string_view{},
                          std::string_view{row.text}, row.link.choice, std::string_view{row.flags}};
    };
    std::ranges::sort(found, [&](const Row& a, const Row& b) { return key(a) < key(b); });
    if (found.empty()) {
        return {};
    }
    std::vector<Row> rows{
        text_row(RowType::note, ""),
        text_row(RowType::heading,
                 std::format("{}, with flags toggled  {}",
                             reverse ? "Would be needed by" : "Would depend on", found.size()))};
    std::ranges::move(found, std::back_inserter(rows));
    return rows;
}

std::vector<Row> page_rows(const Store& store, const Graph& graph, std::uint32_t package) {
    std::vector<Row> rows;
    for (const bool reverse : {false, true}) {
        const auto found = links(store, graph, package, reverse);
        if (reverse) {
            rows.push_back(text_row(RowType::note, ""));
        }
        rows.push_back(
            text_row(RowType::heading,
                     std::format("{}  {}", reverse ? "Needed by" : "Depends on", found.size())));
        if (found.empty()) {
            rows.push_back(text_row(RowType::note, "nothing"));
        }
        for (const auto& link : found) {
            rows.push_back(link_row(link, 0, reverse));
        }
    }
    return rows;
}

// The row of the link a nested row hangs from.
std::optional<std::size_t> parent_of(const std::vector<Row>& rows, std::size_t index) {
    const auto depth = rows.at(index).depth;
    if (rows.at(index).type != RowType::link || depth == 0) {
        return std::nullopt;
    }
    for (std::size_t i = index; i-- > 0;) {
        if (rows.at(i).type == RowType::link && rows.at(i).depth == depth - 1) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

bool available() {
    return EGRAPH_HAVE_TUI != 0;
}

void thread(std::vector<Row>& rows) {
    thread_tree(rows, [](const Row& row) { return selectable(row) ? row.depth : 0; });
}

std::vector<PlanRow> plan_rows(const Store& store, const Evaluated& evaluated, const Kept& kept,
                               const Plan& plan) {
    std::vector<std::vector<std::string>> chains;
    // Each merge by the cpv its chain ends in, which names it wherever it appears.
    std::map<std::string, std::uint32_t, std::less<>> merges;
    // One line per place in the merge order, led by the place.
    const auto lines = update_tree_lines(store, evaluated, kept, plan);
    for (std::size_t place = 0; place < lines.size(); ++place) {
        std::vector<std::string> fields;
        for (const auto field :
             std::views::split(std::string_view{lines.at(place)}, '\t') | std::views::drop(1)) {
            fields.emplace_back(std::string_view{field});
        }
        merges.emplace(fields.back(), plan.order.at(place));
        chains.push_back(std::move(fields));
    }
    struct Node {
        std::string label;
        std::vector<std::size_t> children;
    };
    // Node 0 holds the sets; children keep the order the merges first reach them in.
    std::vector<Node> nodes(1);
    for (const auto& chain : chains) {
        std::size_t at = 0;
        for (const auto& label : chain) {
            const auto& children = nodes.at(at).children;
            const auto found = std::ranges::find_if(
                children, [&](std::size_t child) { return nodes.at(child).label == label; });
            if (found != children.end()) {
                at = *found;
                continue;
            }
            nodes.push_back({.label = label, .children = {}});
            nodes.at(at).children.push_back(nodes.size() - 1);
            at = nodes.size() - 1;
        }
    }
    std::vector<PlanRow> rows;
    const auto walk = [&](this const auto& self, std::size_t index, std::size_t depth) -> void {
        for (const auto child : nodes.at(index).children) {
            const auto& label = nodes.at(child).label;
            const auto merge = merges.find(label);
            rows.push_back({.label = label,
                            .merge = depth > 0 && merge != merges.end()
                                         ? std::optional{merge->second}
                                         : std::nullopt,
                            .depth = depth,
                            .last = false,
                            .rails = {}});
            self(child, depth + 1);
        }
    };
    walk(0, 0);
    thread_tree(rows, [](const PlanRow& row) { return row.depth; });
    return rows;
}

std::vector<Link> links(const Store& store, const Graph& graph, std::uint32_t package,
                        bool reverse) {
    std::vector<Link> found;
    for (const auto& edge : reverse ? graph.rdeps(package) : graph.deps(package)) {
        const auto other = reverse ? edge.parent : edge.child;
        auto link = std::ranges::find_if(found, [&](const Link& candidate) {
            return candidate.package == other && candidate.atom == edge.atom &&
                   candidate.choice == edge.choice;
        });
        if (link == found.end()) {
            found.push_back({.package = other, .atom = edge.atom, .choice = edge.choice});
            link = found.end() - 1;
        }
        if (const auto index = shorthand_of(edge.kind); index < kind_shorthands.size()) {
            link->kinds.at(index) = true;
        }
    }
    std::ranges::sort(found, [&](const Link& a, const Link& b) {
        return std::tuple{store.string(store.packages.at(a.package).cpv), store.string(a.atom),
                          a.choice} < std::tuple{store.string(store.packages.at(b.package).cpv),
                                                 store.string(b.atom), b.choice};
    });
    return found;
}

namespace {

// What an app shows before it owns stores of its own.
const Store& no_store() {
    static const Store none{};
    return none;
}

const Graph& no_graph() {
    static const Graph none = build_graph(no_store());
    return none;
}

const Evaluated& no_evaluated() {
    static const Evaluated none{};
    return none;
}

bool paired(const Store& installed, const Evaluated& evaluated) {
    return !evaluated.packages.empty() && evaluated.packages.size() == installed.packages.size();
}

// The plan's updates, rebuilds and held updates per package, and the held ones' remedies.
} // namespace

std::vector<TriedMerge> tried_merges(std::span<const std::string> records) {
    std::vector<TriedMerge> merges;
    for (const auto& record : records) {
        std::vector<std::string> fields;
        for (const auto field : std::views::split(std::string_view{record}, '\t')) {
            fields.emplace_back(std::string_view{field});
        }
        if (fields.size() < 7 || fields.at(1) != "tried") {
            continue;
        }
        const auto& change = fields.at(2);
        merges.push_back({.first = fields.at(0),
                          .change = change == "added"     ? TriedMerge::Change::added
                                    : change == "changed" ? TriedMerge::Change::changed
                                                          : TriedMerge::Change::dropped,
                          .kind = fields.at(3),
                          .target = fields.at(4),
                          .repo = fields.at(5),
                          .flags = fields.at(6)});
    }
    return merges;
}

namespace {

ScopePlan scoped(Scope scope, Plan plan, const Store& store, const Evaluated& evaluated,
                 const Graph& graph, const Targets& targets, const Rescope& rescope) {
    const auto count = store.packages.size();
    ScopePlan made{.scope = scope,
                   .plan = std::move(plan),
                   .updates = std::vector<std::optional<PendingUpdate>>(count),
                   .rebuilt_for = std::vector<std::string>(count),
                   .held = std::vector<std::optional<std::uint32_t>>(count),
                   .remedies = {},
                   .error = std::nullopt,
                   .generation = 0};
    for (const auto& merge : made.plan.merges) {
        if (!merge.replaces) {
            continue;
        }
        made.updates.at(*merge.replaces) =
            PendingUpdate{.kind = merge.kind, .target = merge.candidate, .flags = merge.flags};
        if (const auto& why = merge.rebuilt_for) {
            made.rebuilt_for.at(*merge.replaces) =
                std::format("{} {}", member_cpv(store, evaluated, why->member), why->atom);
        }
    }
    made.remedies =
        egraph::remedies(store, evaluated, graph, made.plan, shown_rebuilds, targets, rescope);
    for (std::uint32_t i = 0; i < made.plan.held.size(); ++i) {
        made.held.at(made.plan.held.at(i).package) = i;
    }
    return made;
}

// The plan of -uDN on the set, as exec makes it for a set argument: depclean's kept packages
// and what the set reaches in scope, the set's atoms emerge's arguments.
ScopePlan plan_set(Scope scope, const Store& store, const Evaluated& evaluated, const Graph& graph,
                   const std::vector<Masking>& masking, bool dynamic_deps,
                   const std::shared_ptr<const Stores>& untried,
                   const std::vector<std::uint32_t>& reinstall = {}) {
    // With lines tried, what they change against the plan made the same way without them (and
    // without the rebuilds they ask for).
    const auto tried = [&](const Plan& plan, const Targets& targets) {
        if (!untried) {
            return std::vector<TriedMerge>{};
        }
        const auto& before = untried->evaluated;
        auto again = targets;
        again.reinstall.clear();
        return tried_merges(tried_lines(
            update_lines(store, before, plan_updates(store, before, shown_rebuilds, again),
                         shown_rebuilds, HeldLines::none, false, targets),
            update_lines(store, evaluated, plan, shown_rebuilds, HeldLines::none, false, targets)));
    };
    // Every installed package, as index() plans it.
    if (scope == Scope::installed) {
        Targets targets;
        targets.reinstall = reinstall;
        auto made = scoped(scope, plan_updates(store, evaluated, shown_rebuilds, targets), store,
                           evaluated, graph, targets, {});
        made.tried = tried(made.plan, targets);
        return made;
    }
    const auto request =
        parse_request(store, evaluated, std::array{std::string{scope_name(scope)}});
    if (!request) {
        ScopePlan failed = scoped(scope, {}, store, evaluated, graph, {}, {});
        failed.error = std::format("cannot plan {}: {}", scope_name(scope), request.error());
        return failed;
    }
    // An empty set asks for nothing; without arguments the roots would be every root set's.
    if (request->arguments.empty()) {
        return scoped(scope, {}, store, evaluated, graph, {}, {});
    }
    const KeepOptions options{.build_deps = true,
                              .masking = masking,
                              .removed = {},
                              .protect = {},
                              .dropped = {},
                              .without_selected = false};
    Targets targets{.scope = keep(store, options).packages, .roots = true};
    targets.reach = request_reach(store, evaluated, *request);
    for (std::size_t id = 0; id < targets.scope.size(); ++id) {
        targets.scope.at(id) = targets.scope.at(id) || targets.reach.at(id);
    }
    targets.request = request->arguments;
    targets.dynamic_deps = dynamic_deps;
    targets.reinstall = reinstall;
    auto plan = plan_updates(store, evaluated, shown_rebuilds, targets);
    auto changes = tried(plan, targets);
    const Rescope rescope = [&store, &options](const std::vector<bool>& removed) {
        auto without = options;
        without.removed = removed;
        return keep(store, without).packages;
    };
    auto made = scoped(scope, std::move(plan), store, evaluated, graph, targets, rescope);
    made.tried = std::move(changes);
    return made;
}

} // namespace

std::vector<Span> status_spans(const StatusShown& shown, const Glyphs& glyph, Seconds now) {
    const auto& counts = shown.status.counts;
    std::vector<Span> spans{{"@world", tone_pen(Tone::heading)}};
    for (const auto& [count, mark, tone] : {std::tuple{counts.upgrades, glyph.upgrade, Tone::good},
                                            {counts.downgrades, glyph.downgrade, Tone::bad},
                                            {counts.rebuilds, glyph.rebuild, Tone::use},
                                            {counts.added, glyph.added, Tone::good},
                                            {counts.held, glyph.held, Tone::bad}}) {
        if (count != 0) {
            spans.push_back({std::format(" {}{}", mark, count), tone_pen(tone)});
        }
    }
    if (spans.size() == 1) {
        spans.push_back({std::format(" {} up to date", glyph.good), tone_pen(Tone::good)});
    }
    if (counts.refused) {
        spans.push_back({std::format(" {} refused", glyph.broken), tone_pen(Tone::bad)});
    }
    const auto& repositories = shown.status.repositories;
    const RepositorySync* oldest = nullptr;
    for (const auto& repository : repositories) {
        if (repository.synced && (!oldest || repository.synced < oldest->synced)) {
            oldest = &repository;
        }
    }
    if (oldest) {
        spans.push_back({std::format("  {} synced {}", oldest->name,
                                     age_text(oldest->synced.value_or(now), now)),
                         tone_pen(Tone::note)});
    }
    if (!shown.current) {
        spans.push_back({"  older than the stores", tone_pen(Tone::bad)});
    }
    spans.push_back({" ", {}});
    return spans;
}

std::string_view scope_name(Scope scope) {
    switch (scope) {
    case Scope::installed:
        return "@installed";
    case Scope::world:
        return "@world";
    case Scope::system:
        return "@system";
    }
    return "@installed";
}

App::Loaded::Loaded(std::shared_ptr<const Stores> shared, bool dynamic_deps)
    : stores(std::move(shared)),
      dynamic(dynamic_deps && paired(stores->installed, stores->evaluated)
                  ? std::optional<Store>{with_dynamic_deps(stores->installed, stores->evaluated)}
                  : std::nullopt),
      graph(build_graph(dynamic ? *dynamic : stores->installed)) {}

App::App(const Store& store, const Graph& graph, Update update)
    : store_(store), installed_(store), evaluated_(no_evaluated()), graph_(graph), update_(update) {
    index();
    recompute();
    filter();
}

App::App(Stores stores, bool dynamic_deps, Update update)
    : App(std::make_shared<const Stores>(std::move(stores)), dynamic_deps, update) {}

App::App(std::shared_ptr<const Stores> stores, bool dynamic_deps, Update update)
    : App(no_store(), no_graph(), update) {
    dynamic_deps_ = dynamic_deps;
    own(std::make_shared<const Loaded>(std::move(stores), dynamic_deps));
    index();
    recompute();
    // What there is to do first, when the evaluated store says.
    if (has_evaluated()) {
        list_.only = Only::updates;
    }
    filter();
}

void App::own(std::shared_ptr<const Loaded> loaded) {
    // It reads the stores being replaced; index() makes it anew.
    catalogue_.reset();
    store_ = loaded->dynamic ? *loaded->dynamic : loaded->stores->installed;
    installed_ = loaded->stores->installed;
    evaluated_ = loaded->stores->evaluated;
    graph_ = loaded->graph;
    owned_ = std::move(loaded);
}

void App::index() {
    const auto& store = this->store();
    const auto& graph = graph_.get();
    const auto count = store.packages.size();
    folded_.clear();
    folded_.reserve(count);
    dependencies_.assign(count, 0);
    dependents_.assign(count, 0);
    broken_.clear();
    broken_.reserve(count);
    broken_at_run_time_.clear();
    broken_at_run_time_.reserve(count);
    for (std::uint32_t id = 0; id < count; ++id) {
        folded_.push_back(folded(store.string(store.packages.at(id).cpv)));
        std::vector<std::uint32_t> children;
        for (const auto& edge : graph.deps(id)) {
            children.push_back(edge.child);
        }
        std::ranges::sort(children);
        const auto [first, last] = std::ranges::unique(children);
        children.erase(first, last);
        dependencies_.at(id) = children.size();
        std::size_t broken = 0;
        std::size_t at_run_time = 0;
        for (const auto& dependency : unsatisfied(store, id)) {
            if (!is_build_kind(dependency.kind)) {
                ++at_run_time;
                ++broken;
            } else if (!replaced(dependency, installed_instead(store, dependency))) {
                ++broken;
            }
        }
        broken_.push_back(broken);
        broken_at_run_time_.push_back(at_run_time);
        for (const auto child : children) {
            ++dependents_.at(child);
        }
    }
    masking_.clear();
    ++generation_;
    plans_ = {};
    blank_ = {.scope = scope_,
              .plan = {},
              .updates = std::vector<std::optional<PendingUpdate>>(count),
              .rebuilt_for = std::vector<std::string>(count),
              .held = std::vector<std::optional<std::uint32_t>>(count),
              .remedies = {},
              .error = std::nullopt,
              .generation = generation_};
    if (has_evaluated()) {
        masking_.reserve(count);
        for (std::uint32_t id = 0; id < count; ++id) {
            const auto& pkg = evaluated().packages.at(id);
            masking_.push_back(
                {.masked = dynamic_deps_ ? pkg.masked : pkg.vdb_masked, .visible = pkg.visible});
        }
    }
    // The first page's, at once; the others' in the background once shown, as are all with lines
    // tried, planned twice.
    if (has_evaluated() && !untried_) {
        auto installed =
            plan_set(Scope::installed, store, evaluated(), graph, masking_, dynamic_deps_, nullptr);
        installed.generation = generation_;
        plans_.front() = std::move(installed);
    }
    if (indexed_ && !catalogue_) {
        catalogue_.emplace(installed(), evaluated(), *indexed_->index, indexed_->masks);
    }
    std::erase_if(picked_, [this](const std::string& cpv) { return !find(cpv); });
}

void App::adopt(std::shared_ptr<const Stores> stores, Source source) {
    rebase(stores);
    own(std::make_shared<const Loaded>(std::move(stores), dynamic_deps_));
    source_ = source;
    // They were built after any refresh asked for.
    stale_.reset();
    pages_.clear();
    index();
    recompute();
    filter();
}

std::shared_ptr<const Stores> App::shared() const {
    if (untried_) {
        return untried_;
    }
    return owned_ ? owned_->stores : nullptr;
}

void App::rebase(const std::shared_ptr<const Stores>& stores) {
    if (lines_.empty()) {
        return;
    }
    untried_ = stores;
    ++version_;
}

TryRequest App::start_try() {
    trying_ = true;
    try_version_ = version_;
    return {.stores = shared(), .lines = lines_};
}

void App::finish_try(TryResult result) {
    trying_ = false;
    if (try_version_ != version_) {
        return;
    }
    tried_version_ = try_version_;
    if (!result) {
        show({.error = true, .title = "Not tried", .lines = {std::move(result.error())}});
        return;
    }
    if (!result->unevaluated.empty()) {
        std::string cps;
        for (const auto& cp : result->unevaluated) {
            cps += (cps.empty() ? "" : ", ") + cp;
        }
        show({.error = true,
              .title = "Not evaluated",
              .lines = {std::format("{}: reached by what is tried, but not evaluated "
                                    "(--no-refresh)",
                                    cps)}});
    }
    untried_ = std::move(result->untried);
    replace(std::move(result->tried));
    env_changes_ = egraph::env_changes(installed(), untried(), evaluated(), lines_);
    rebuild_env_ = rebuild_env_ && !env_changes_.empty();
}

void App::toggle(const FlagState& flag, const std::string& atom) {
    auto lines = lines_;
    auto line = std::ranges::find_if(lines, [&](const WhatIfLine& each) {
        return each.file == WhatIfLine::File::use && each.atom == atom;
    });
    const auto off = "-" + flag.flag;
    if (line != lines.end() &&
        (std::erase(line->tokens, flag.flag) + std::erase(line->tokens, off)) > 0) {
        if (line->tokens.empty()) {
            lines.erase(line);
        }
    } else if (line != lines.end()) {
        line->tokens.push_back(flag.enabled ? off : flag.flag);
    } else {
        lines.push_back({.file = WhatIfLine::File::use,
                         .atom = atom,
                         .tokens = {flag.enabled ? off : flag.flag}});
    }
    set_lines(std::move(lines));
}

void App::set_lines(std::vector<WhatIfLine> lines, bool undoable) {
    if (undoable) {
        undo_.push_back(lines_);
    }
    if (lines_.empty() && !lines.empty()) {
        on_what_if_ = true;
        on_notices_ = false;
    }
    lines_ = std::move(lines);
    what_if_cursor_.at = std::min(what_if_cursor_.at, lines_.empty() ? 0 : lines_.size() - 1);
    ++version_;
    // run() drops a try made for no lines.
    trying_ = trying_ && !lines_.empty();
    if (lines_.empty()) {
        on_what_if_ = false;
        env_changes_.clear();
        rebuild_env_ = false;
        if (untried_) {
            auto untried = std::move(untried_);
            untried_.reset();
            tried_version_ = version_;
            replace(std::move(untried));
        }
    }
}

void App::replan() {
    ++generation_;
    plans_ = {};
    if (list_.only == Only::updates) {
        filter();
    }
}

std::vector<std::uint32_t> App::reinstalled() const {
    std::vector<std::uint32_t> ids;
    if (rebuild_env_) {
        for (const auto& change : env_changes_) {
            ids.push_back(change.package);
        }
        std::ranges::sort(ids);
    }
    return ids;
}

void App::finish_stale_check(std::optional<std::string> reason) {
    stale_ = std::move(reason);
}

void App::finish_refresh(RefreshResult result) {
    stale_.reset();
    if (!result) {
        refresh_error_ = std::move(result.error());
        return;
    }
    refresh_error_.reset();
    rebase(*result);
    replace(std::move(*result));
}

std::optional<std::uint32_t> App::find_again(std::string_view cpv, std::string_view cp,
                                             std::string_view slot) const {
    if (const auto same = find(cpv)) {
        return same;
    }
    const auto& store = this->store();
    std::optional<std::uint32_t> named;
    for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
        const auto& pkg = store.packages.at(id);
        if (store.string(pkg.cp) != cp) {
            continue;
        }
        if (store.string(pkg.slot) == slot) {
            return id;
        }
        named = named.value_or(id);
    }
    return named;
}

void App::replace(std::shared_ptr<const Stores> stores) {
    // Where each view is, by names that outlast the old stores.
    struct Place {
        std::string cpv;
        std::string cp;
        std::string slot;
    };
    const auto place_of = [this](std::uint32_t id) {
        const auto& pkg = store().packages.at(id);
        return Place{.cpv = std::string{store().string(pkg.cpv)},
                     .cp = std::string{store().string(pkg.cp)},
                     .slot = std::string{store().string(pkg.slot)}};
    };
    struct Opened {
        Place package;
        Cursor cursor;
        // The selected row, when it names a package.
        std::optional<Place> row;
        RowType type = RowType::note;
        std::size_t depth = 0;
        bool reverse = false;
        // The selected row's flag, on a flag row.
        std::optional<std::string> flag{};
    };
    const auto list_cursor = list_.cursor;
    const auto plan_cursor = planned_ ? planned_->cursor : Cursor{};
    const auto plan_label = planned_ && plan_cursor.at < planned_->rows.size()
                                ? std::optional{planned_->rows.at(plan_cursor.at).label}
                                : std::nullopt;
    const auto listed = list_cursor.at < list_.shown.size()
                            ? std::optional{place_of(list_.shown.at(list_cursor.at))}
                            : std::nullopt;
    std::vector<Opened> opened;
    for (const auto& page : pages_) {
        Opened entry{.package = place_of(page.package), .cursor = page.cursor, .row = std::nullopt};
        if (page.cursor.at < page.rows.size()) {
            const auto& row = page.rows.at(page.cursor.at);
            entry.type = row.type;
            entry.depth = row.depth;
            entry.reverse = row.reverse;
            if (selectable(row)) {
                entry.row = place_of(row.link.package);
            }
            if (row.flag) {
                entry.flag = row.flag->flag;
            }
        }
        opened.push_back(std::move(entry));
    }

    own(std::make_shared<const Loaded>(std::move(stores), dynamic_deps_));
    index();
    recompute();
    filter();

    // The same distance from the top of the view as before.
    const auto restore = [](Cursor& cursor, const Cursor& was, std::size_t at) {
        cursor.at = at;
        cursor.top = at - std::min(at, was.at - was.top);
    };
    const auto found = [&](const std::optional<Place>& place) -> std::optional<std::uint32_t> {
        return place ? find_again(place->cpv, place->cp, place->slot) : std::nullopt;
    };
    if (!list_.shown.empty()) {
        const auto id = found(listed);
        const auto at = id ? std::ranges::find(list_.shown, *id) : list_.shown.end();
        restore(list_.cursor, list_cursor,
                at != list_.shown.end() ? static_cast<std::size_t>(at - list_.shown.begin())
                                        : std::min(list_cursor.at, list_.shown.size() - 1));
    }
    if (planned_) {
        auto& planned = open_plan(plan_label);
        const auto& rows = planned.rows;
        if (!rows.empty()) {
            auto at = planned.cursor.at;
            // What it was on is gone: the package at or above the same place.
            if (rows.at(at).label != plan_label) {
                at = std::min(plan_cursor.at, rows.size() - 1);
                while (at > 0 && rows.at(at).depth == 0) {
                    --at;
                }
                if (rows.at(at).depth == 0) {
                    at = planned.cursor.at;
                }
            }
            restore(planned.cursor, plan_cursor, at);
        }
    }
    pages_.clear();
    for (const auto& entry : opened) {
        const auto id = found(entry.package);
        if (!id) {
            continue;
        }
        open(*id);
        auto& page = pages_.back();
        const auto row = found(entry.row);
        const auto same = std::ranges::find_if(page.rows, [&](const Row& candidate) {
            return row && selectable(candidate) && candidate.type == entry.type &&
                   candidate.depth == entry.depth && candidate.reverse == entry.reverse &&
                   candidate.link.package == *row;
        });
        const auto flag = std::ranges::find_if(page.rows, [&](const Row& candidate) {
            return entry.flag && candidate.flag && candidate.flag->flag == *entry.flag;
        });
        if (same != page.rows.end()) {
            restore(page.cursor, entry.cursor, static_cast<std::size_t>(same - page.rows.begin()));
        } else if (flag != page.rows.end()) {
            restore(page.cursor, entry.cursor, static_cast<std::size_t>(flag - page.rows.begin()));
        } else if (!row && !page.rows.empty()) {
            restore(page.cursor, entry.cursor, std::min(entry.cursor.at, page.rows.size() - 1));
        }
    }
    if (output_) {
        for (std::size_t i = 0; i < output_->rows.size(); ++i) {
            output_->links.at(i) = link_of(output_->rows.at(i));
        }
    }
    if (search_ && search_->ran) {
        const auto was = search_->cursor;
        run_search();
        if (!search_->results.empty()) {
            restore(search_->cursor, was, std::min(was.at, search_->results.size() - 1));
        }
    }
    if (listing_ && catalogue_ && catalogue_->contains(listing_->found.cp)) {
        const auto was = listing_->cursor;
        open_listing(catalogue_->found(listing_->found.cp));
        if (!listing_->rows.empty()) {
            listing_->cursor = was;
            listing_->cursor.at = std::min(was.at, listing_->rows.size() - 1);
        }
    }
}

std::optional<std::uint32_t> App::link_of(const std::vector<std::string>& fields) const {
    for (const auto& field : fields) {
        if (const auto id = find(field)) {
            return id;
        }
    }
    return std::nullopt;
}

void App::recompute() {
    kept_ = keep(store(), {.build_deps = build_deps_,
                           .masking = masking_,
                           .removed = {},
                           .protect = {},
                           .dropped = {},
                           .without_selected = false});
    root_of_.assign(store().packages.size(), std::nullopt);
    for (const auto& pull : kept_.roots) {
        auto& root = root_of_.at(pull.child);
        if (!root || pull.root < *root) {
            root = pull.root;
        }
    }
}

const ScopePlan& App::current() const {
    const auto& plan = plans_.at(static_cast<std::size_t>(scope_));
    return plan ? *plan : blank_;
}

bool App::scope_planned(Scope scope) const {
    return plans_.at(static_cast<std::size_t>(scope)).has_value();
}

bool App::scope_plan_requested() const {
    return planning_ || (has_evaluated() && !scope_planned(scope_));
}

Job<ScopePlan> App::start_scope_plan() {
    planning_ = scope_;
    // Copies and shared stores, as the app may move on to others meanwhile.
    auto made =
        std::async(std::launch::async,
                   [scope = scope_, loaded = owned_, store = store_, evaluated = evaluated_,
                    graph = graph_, masking = masking_, dynamic_deps = dynamic_deps_,
                    generation = generation_, untried = untried_, reinstall = reinstalled()] {
                       auto plan = plan_set(scope, store.get(), evaluated.get(), graph.get(),
                                            masking, dynamic_deps, untried, reinstall);
                       plan.generation = generation;
                       return plan;
                   });
    return [made = std::move(made)]() mutable -> std::optional<ScopePlan> {
        if (made.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return std::nullopt;
        }
        return made.get();
    };
}

void App::finish_scope_plan(ScopePlan plan) {
    planning_.reset();
    if (plan.generation != generation_) {
        return;
    }
    if (plan.error) {
        show({.error = true, .title = "No plan", .lines = {*plan.error}});
    }
    const auto scope = plan.scope;
    plans_.at(static_cast<std::size_t>(scope)) = std::move(plan);
    if (scope == scope_ && list_.only == Only::updates) {
        filter();
    }
}

void App::open_notices() {
    notices_first_ = !notices_;
    on_notices_ = notices_.has_value();
}

void App::finish_notices(std::optional<NoticesShown> notices) {
    std::optional<std::string> selected;
    if (notices_ && notice_cursor_.at < notices_->notices.size()) {
        selected = notices_->notices.at(notice_cursor_.at).key;
    }
    notices_ = std::move(notices);
    if (!notices_) {
        on_notices_ = false;
        putting_off_ = false;
        notice_cursor_ = {};
        return;
    }
    on_notices_ = on_notices_ || std::exchange(notices_first_, false);
    const auto& list = notices_->notices;
    if (const auto at = std::ranges::find(list, selected, &Notice::key); at != list.end()) {
        notice_cursor_.at = static_cast<std::size_t>(at - list.begin());
    }
    notice_cursor_.at = std::min(notice_cursor_.at, list.empty() ? 0 : list.size() - 1);
    keep_visible(notice_cursor_, height_);
}

void App::finish_notice_change(const std::expected<void, std::string>& result) {
    if (!notice_change_) {
        return;
    }
    auto change = std::move(*notice_change_);
    notice_change_.reset();
    if (!result && change.read) {
        show({.error = false,
              .title = "Set aside for you alone",
              .lines = {"Portage's news files cannot be marked:", result.error(),
                        "eselect news still counts it unread."}});
        notice_change_ = NoticeChange{.notice = std::move(change.notice)};
        return;
    }
    if (!result) {
        show({.error = true,
              .title = change.later ? "Cannot put the notice off" : "Cannot dismiss the notice",
              .lines = {result.error()}});
        return;
    }
    const auto& key = change.notice.key;
    if (notices_ && std::erase_if(notices_->notices,
                                  [&key](const Notice& notice) { return notice.key == key; })) {
        notices_->set_aside += change.read ? 0 : 1;
        auto again = std::move(notices_);
        finish_notices(std::move(again));
    }
}

void App::finish_news(const std::expected<std::vector<std::string>, std::string>& text) {
    if (!news_wanted_) {
        return;
    }
    auto news = std::move(*news_wanted_);
    news_wanted_.reset();
    if (!text) {
        show({.error = true, .title = "Cannot read the news item", .lines = {text.error()}});
        return;
    }
    show({.error = false,
          .title = news.title,
          .lines = *text,
          .question = false,
          .top = 0,
          .closing = " any key marks it read "});
    reading_ = std::move(news);
}

void App::finish_dispatch_conf(const std::expected<int, std::string>& ran) {
    if (!dispatching_) {
        return;
    }
    dispatching_ = false;
    if (!ran) {
        show({.error = true, .title = "Cannot run dispatch-conf", .lines = {ran.error()}});
    } else if (*ran != 0) {
        show({.error = true,
              .title = std::format("dispatch-conf exited with status {}", *ran),
              .lines = {}});
    }
}

void App::handle_notices(const Key& key) {
    const auto count = notices_ ? notices_->notices.size() : 0;
    const bool on_notice =
        notice_cursor_.at < count && !notice_change_ && !news_wanted_ && !dispatching_;
    if (putting_off_) {
        if (key.kind == KeyKind::escape) {
            putting_off_ = false;
        } else if (key.kind == KeyKind::character && key.code >= U'1' &&
                   key.code - U'1' < put_off_choices.size()) {
            putting_off_ = false;
            if (on_notice) {
                notice_change_ = NoticeChange{
                    .notice = notices_->notices.at(notice_cursor_.at),
                    .later = put_off_choices.at(static_cast<std::size_t>(key.code - U'1')).second};
            }
        }
        return;
    }
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::left || is(key, U'h')) {
        turn_page(-1);
    } else if (key.kind == KeyKind::right || is(key, U'l')) {
        turn_page(1);
    } else if (is(key, U'x') && on_notice) {
        notice_change_ = NoticeChange{.notice = notices_->notices.at(notice_cursor_.at)};
    } else if (is(key, U'z') && on_notice) {
        putting_off_ = true;
    } else if (key.kind == KeyKind::enter && on_notice) {
        work_on(notices_->notices.at(notice_cursor_.at));
    } else if (is_move(key)) {
        move(notice_cursor_, count, key, height_);
    }
}

void App::handle_typed_line(TypedLine& typed, const Key& key) {
    if (key.kind == KeyKind::character && key.code >= U' ') {
        append_utf8(typed.text, key.code);
    } else if (key.kind == KeyKind::backspace && !typed.text.empty()) {
        pop_code_point(typed.text);
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace) {
        typed_line_.reset();
    } else if (key.kind == KeyKind::enter) {
        auto line = parse_what_if(typed.file, typed.text);
        if (!line) {
            show({.error = true, .title = "Not a line", .lines = {std::move(line.error())}});
            return;
        }
        typed_line_.reset();
        auto lines = lines_;
        lines.push_back(std::move(*line));
        set_lines(std::move(lines));
        what_if_cursor_.at = lines_.size() - 1;
        keep_visible(what_if_cursor_, height_);
    }
}

void App::handle_what_if(const Key& key) {
    const bool on_line = what_if_cursor_.at < lines_.size();
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::left || is(key, U'h')) {
        turn_page(-1);
    } else if (key.kind == KeyKind::right || is(key, U'l')) {
        turn_page(1);
    } else if (is(key, U'x') && on_line) {
        auto lines = lines_;
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(what_if_cursor_.at));
        set_lines(std::move(lines));
    } else if (is(key, U'u') && !undo_.empty()) {
        auto lines = std::move(undo_.back());
        undo_.pop_back();
        set_lines(std::move(lines), false);
    } else if (is(key, U'a')) {
        typed_line_ = TypedLine{.file = WhatIfLine::File::use, .text = {}};
    } else if (is(key, U'e')) {
        typed_line_ = TypedLine{.file = WhatIfLine::File::env, .text = {}};
    } else if (is(key, U'r') && !env_changes_.empty()) {
        rebuild_env_ = !rebuild_env_;
        replan();
    } else if (is_move(key)) {
        move(what_if_cursor_, lines_.size(), key, height_);
    }
}

void App::work_on(const Notice& notice) {
    if (notice.kind == NoticeKind::masked && !notice.packages.empty()) {
        const auto& cpv = notice.packages.front();
        if (const auto id = find(cpv)) {
            open(*id);
        } else {
            show({.error = false,
                  .title = std::format("{} is no longer installed", cpv),
                  .lines = {}});
        }
    } else if (notice.kind == NoticeKind::news && !notice.file.empty()) {
        news_wanted_ = notice;
    } else if (notice.kind == NoticeKind::config) {
        dispatching_ = true;
    } else if (notice.kind == NoticeKind::check) {
        command_ = "config check";
    } else if (auto action = notice_action(notice)) {
        act(std::move(*action));
    }
}

void App::turn_page(int step) {
    const std::size_t sets = has_evaluated() ? scopes.size() : 1;
    const auto notices = static_cast<int>(sets);
    const auto what_if = notices + (notices_ ? 1 : 0);
    const auto count = what_if + (lines_.empty() ? 0 : 1);
    const int shown = on_what_if()      ? what_if
                      : on_notices_     ? notices
                      : has_evaluated() ? static_cast<int>(scope_)
                                        : 0;
    const auto at = shown + step;
    if (at < 0 || at >= count) {
        return;
    }
    on_what_if_ = !lines_.empty() && at == what_if;
    on_notices_ = notices_.has_value() && at == notices;
    if (on_what_if_ || on_notices_ || !has_evaluated()) {
        return;
    }
    scope_ = scopes.at(static_cast<std::size_t>(at));
    if (list_.only == Only::updates) {
        filter();
    }
}

void App::filter() {
    const auto query = folded(list_.query);
    list_.shown.clear();
    for (std::uint32_t id = 0; id < folded_.size(); ++id) {
        if ((list_.only == Only::orphans && kept_.packages.at(id)) ||
            (list_.only == Only::broken && broken(id) == 0) ||
            (list_.only == Only::updates && !update_of(id) && !held_of(id) &&
             !tried_of(store().string(store().packages.at(id).cpv)))) {
            continue;
        }
        if (folded_.at(id).find(query) != std::string::npos) {
            list_.shown.push_back(id);
        }
    }
    list_.cursor = {};
}

void App::open(std::uint32_t package) {
    Page page{
        .package = package, .rows = kept_rows(store(), kept_, package, build_deps_), .cursor = {}};
    const auto& planned = current();
    std::ranges::move(update_rows(planned.updates.at(package), planned.rebuilt_for.at(package)),
                      std::back_inserter(page.rows));
    if (const auto held = planned.held.at(package)) {
        std::ranges::move(
            held_rows(store(), graph_.get(), evaluated(), planned.plan, planned.remedies.at(*held)),
            std::back_inserter(page.rows));
    }

    std::ranges::move(unsatisfied_rows(store(), package, build_deps_),
                      std::back_inserter(page.rows));
    std::ranges::move(page_rows(store(), graph_.get(), package), std::back_inserter(page.rows));
    if (has_evaluated()) {
        for (const bool reverse : {false, true}) {
            std::ranges::move(possible_rows(store(), evaluated(), package, reverse),
                              std::back_inserter(page.rows));
        }
    }
    if (has_evaluated()) {
        std::ranges::move(use_rows(installed(), evaluated(), untried(), package),
                          std::back_inserter(page.rows));
    }
    // Last, so that what keeps the package and its dependencies open in view.
    if (auto versions = version_rows(store().string(store().packages.at(package).cp));
        !versions.empty()) {
        page.rows.push_back(text_row(RowType::note, ""));
        std::ranges::move(versions, std::back_inserter(page.rows));
    }
    thread(page.rows);
    // On the first dependency, else on whatever can be selected.
    const auto first = std::ranges::find(page.rows, RowType::link, &Row::type);
    const auto any = std::ranges::find_if(page.rows, selectable);
    page.cursor.at = static_cast<std::size_t>((first != page.rows.end() ? first
                                               : any != page.rows.end() ? any
                                                                        : page.rows.begin()) -
                                              page.rows.begin());
    keep_visible(page.cursor, height_);
    if (page.cursor.at < height_) {
        page.cursor.top = 0;
    }
    pages_.push_back(std::move(page));
}

bool App::typing() const {
    if (typed_line_) {
        return true;
    }
    if (!pages_.empty() || listing_ || output_) {
        return false;
    }
    if (search_) {
        return search_->typing;
    }
    return list_.searching && !checked_ && !watched_ && !planned_;
}

void App::handle(const Key& key) {
    const bool answering = dialog_.has_value();
    if (key.kind == KeyKind::closed) {
        done_ = true;
    } else if (key.kind == KeyKind::tick) {
        if (watched_ && pages_.empty()) {
            watched_->due = true;
        }
        if (checked_ && (checked_->stage == Checked::Stage::checking ||
                         checked_->stage == Checked::Stage::rebuilding)) {
            ++checked_->frame;
        }
        if (refresh_requested() || index_requested() || running_ || planning_ || trying_) {
            ++frame_;
        }
    } else if (keys_shown_) {
        keys_shown_ = false;
    } else if (dialog_) {
        handle_dialog(key);
    } else if (putting_off_) {
        handle_notices(key);
    } else if (prompt_) {
        handle_prompt(*prompt_, key);
    } else if (typed_line_) {
        handle_typed_line(*typed_line_, key);
    } else if (is(key, U':') && !typing()) {
        prompt_.emplace();
    } else if (is(key, U'?') && !typing()) {
        keys_shown_ = true;
    } else if (!pages_.empty()) {
        handle_page(key);
        // Back at the emerge view, whatever it shows is stale.
        if (pages_.empty() && !output_ && watched_) {
            watched_->due = true;
            watched_->urgent = true;
        }
    } else if (listing_) {
        handle_listing(key);
    } else if (output_) {
        handle_output(*output_, key);
    } else if (search_) {
        handle_search(key);
    } else if (checked_) {
        handle_check(key);
    } else if (watched_) {
        handle_watch(key);
    } else if (planned_) {
        handle_plan(key);
    } else if (on_what_if()) {
        handle_what_if(key);
    } else if (on_notices_) {
        handle_notices(key);
    } else {
        handle_list(key);
    }
    if (done_ && running_ && key.kind != KeyKind::closed && !answering) {
        done_ = false;
        show({.error = false,
              .title = "A run is going",
              .lines = {"It goes on after egraph quits, in the emerge view of the next one.",
                        "Quit?"},
              .question = true});
    }
}

void App::handle_dialog(const Key& key) {
    if (!dialog_) {
        return;
    }
    auto& dialog = *dialog_;
    const auto last =
        dialog.lines.size() > dialog_height_ ? dialog.lines.size() - dialog_height_ : 0;
    if (last > 0 && is_move(key)) {
        if (key.kind == KeyKind::up || is(key, U'k')) {
            dialog.top -= std::min<std::size_t>(dialog.top, 1);
        } else if (key.kind == KeyKind::down || is(key, U'j')) {
            dialog.top = std::min(dialog.top + 1, last);
        } else if (key.kind == KeyKind::page_up) {
            dialog.top -= std::min(dialog.top, dialog_height_);
        } else if (key.kind == KeyKind::page_down) {
            dialog.top = std::min(dialog.top + dialog_height_, last);
        } else if (key.kind == KeyKind::home || is(key, U'g')) {
            dialog.top = 0;
        } else {
            dialog.top = last;
        }
        return;
    }
    if (!dialog.question) {
        dialog_.reset();
        if (reading_) {
            notice_change_ = NoticeChange{.notice = std::move(*reading_), .read = true};
            reading_.reset();
        }
        return;
    }
    if (is(key, U'y') || is(key, U'Y')) {
        dialog_.reset();
        if (!confirming_) {
            done_ = true;
            return;
        }
        running_ = std::move(confirming_);
        confirming_.reset();
        picked_.clear();
        // The run shows in the emerge view.
        pages_.clear();
        listing_.reset();
        output_.reset();
        search_.reset();
        checked_.reset();
        if (watched_) {
            watched_->due = true;
            watched_->urgent = true;
        } else {
            watched_.emplace();
        }
    } else if (is(key, U'n') || is(key, U'N') || is(key, U'q') || is(key, U'Q') ||
               key.kind == KeyKind::escape) {
        dialog_.reset();
        confirming_.reset();
    }
}

void App::act(Action action) {
    if (running_) {
        show({.error = false,
              .title = "A run is going",
              .lines = {"One runs at a time; the emerge view shows this one."}});
        return;
    }
    if (!previewed(action)) {
        std::string command = "egraph";
        for (const auto& word : action_arguments(action)) {
            command += ' ' + word;
        }
        show({.error = false,
              .title = std::format("Run egraph {}?", action_arguments(action).front()),
              .lines = {std::move(command)},
              .question = true});
        confirming_ = std::move(action);
        return;
    }
    previewing_ = std::move(action);
}

void App::install(std::string_view cp, const PackageVersion& version) {
    const auto cpv = std::format("{}-{}", cp, version.version);
    if (!version.ebuild) {
        show({.error = false,
              .title = std::format("No repository holds {}", cpv),
              .lines = {"Only an ebuild can be installed."}});
        return;
    }
    act({.kind = Action::Kind::install,
         .targets = {std::format("={}::{}", cpv, version.repo)},
         .build_deps = true});
}

namespace {

// Text's lines without the blank ones at its end.
std::vector<std::string> trimmed_lines(std::string_view text) {
    auto found = lines(text);
    while (!found.empty() && found.back().empty()) {
        found.pop_back();
    }
    return found;
}

} // namespace

void App::finish_preview(const Preview& preview) {
    auto action = std::move(previewing_);
    previewing_.reset();
    if (!action) {
        return;
    }
    auto shown = trimmed_lines(preview.out);
    const auto problems = trimmed_lines(preview.err);
    if (!shown.empty() && !problems.empty()) {
        shown.emplace_back();
    }
    shown.insert(shown.end(), problems.begin(), problems.end());
    if (preview.ready) {
        const auto title = std::format("Run egraph {}?", action_arguments(*action).front());
        confirming_ = std::move(action);
        show({.error = false, .title = title, .lines = std::move(shown), .question = true});
    } else {
        show({.error = !problems.empty(),
              .title = problems.empty() ? "Nothing to do" : "It cannot run",
              .lines = std::move(shown)});
    }
}

void App::finish_run(RunResult result) {
    auto action = std::move(running_);
    running_.reset();
    if (!action) {
        return;
    }
    const auto command = std::format("egraph {}", action_arguments(*action).front());
    if (!result.error.empty()) {
        show({.error = true,
              .title = std::format("{} could not start", command),
              .lines = lines(result.error)});
        return;
    }
    if (result.status == 0) {
        show({.error = false, .title = std::format("{} finished", command), .lines = {}});
        return;
    }
    std::vector<std::string> shown;
    for (const auto& failure : result.failures) {
        shown.push_back(failure.log.empty()
                            ? std::format("{} failed", failure.cpv)
                            : std::format("{} failed; its log, {}:", failure.cpv, failure.log));
        for (const auto& line : failure.tail) {
            shown.push_back(std::format("  {}", line));
        }
        shown.emplace_back();
    }
    shown.push_back(std::format("{} exited with status {}; its output, {}:", command, result.status,
                                result.output));
    for (const auto& line : result.tail) {
        shown.push_back(std::format("  {}", line));
    }
    show({.error = true, .title = std::format("{} failed", command), .lines = std::move(shown)});
}

namespace {

std::vector<std::string> nonempty_lines(std::string_view text) {
    std::vector<std::string> lines;
    for (const auto line : std::views::split(text, '\n')) {
        if (!std::string_view{line}.empty()) {
            lines.emplace_back(std::string_view{line});
        }
    }
    return lines;
}

} // namespace

void App::finish_command(const Answer& answer) {
    if (!command_) {
        return;
    }
    auto command = std::move(*command_);
    command_.reset();
    if (answer.quit) {
        done_ = true;
        return;
    }
    const auto problems = nonempty_lines(answer.err);
    const bool failed = answer.exit != Exit::ok;
    if (failed && answer.out.empty()) {
        dialog_ = Dialog{.error = true,
                         .title = std::format(":{} failed", command),
                         .lines = problems.empty() ? std::vector<std::string>{"It printed nothing."}
                                                   : problems};
        return;
    }
    Output output{.command = std::move(command), .rows = {}, .links = {}, .cursor = {}};
    for (const auto& line : nonempty_lines(answer.out)) {
        std::vector<std::string> fields;
        for (const auto field : std::views::split(std::string_view{line}, '\t')) {
            fields.emplace_back(std::string_view{field});
        }
        output.links.push_back(link_of(fields));
        output.rows.push_back(std::move(fields));
    }
    pages_.clear();
    listing_.reset();
    output_ = std::move(output);
    if (!problems.empty()) {
        dialog_ =
            Dialog{.error = failed, .title = failed ? "Errors" : "Warnings", .lines = problems};
    }
}

void App::handle_prompt(std::string& text, const Key& key) {
    if (key.kind == KeyKind::character && key.code >= U' ') {
        append_utf8(text, key.code);
    } else if (key.kind == KeyKind::backspace) {
        if (text.empty()) {
            prompt_.reset();
        } else {
            pop_code_point(text);
        }
    } else if (key.kind == KeyKind::escape) {
        prompt_.reset();
    } else if (key.kind == KeyKind::enter) {
        constexpr std::string_view space = " \t";
        const auto first = text.find_first_not_of(space);
        if (first != std::string::npos) {
            command_ = text.substr(first, text.find_last_not_of(space) - first + 1);
        }
        prompt_.reset();
    }
}

void App::handle_output(Output& output, const Key& key) {
    if (is(key, U'q') || key.kind == KeyKind::escape || key.kind == KeyKind::left ||
        is(key, U'h')) {
        output_.reset();
        if (watched_) {
            watched_->due = true;
            watched_->urgent = true;
        }
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (output.cursor.at < output.links.size()) {
            if (const auto link = output.links.at(output.cursor.at)) {
                open(*link);
            }
        }
    } else if (is_move(key)) {
        move(output.cursor, output.rows.size(), key, height_);
    }
}

void App::finish_check(CheckResult result) {
    if (!checked_) {
        return;
    }
    checked_->stage = Checked::Stage::checked;
    checked_->cursor = {};
    if (result) {
        checked_->drift = std::move(result->drift);
        if (update_ == Update::preview && !checked_->drift.empty()) {
            checked_->fresh = Stores{.installed = std::move(result->store),
                                     .evaluated = std::move(result->evaluated)};
        }
    } else {
        checked_.reset();
        show({.error = true, .title = "The check could not run", .lines = lines(result.error())});
    }
}

void App::finish_rebuild(RebuildResult result) {
    if (!checked_) {
        return;
    }
    if (!result) {
        // The drift still stands, and u can try again.
        checked_->stage = Checked::Stage::checked;
        show({.error = true,
              .title = "The rebuild failed; the store is as it was",
              .lines = lines(result.error())});
        return;
    }
    checked_->stage = Checked::Stage::rebuilt;
    checked_->cursor = {};
    checked_->drift.clear();
    adopt(std::move(*result), Source::saved);
}

bool App::watch_requested() const {
    return watched_ && watched_->due && pages_.empty();
}

void App::finish_watch(std::vector<emerge::Snapshot> snapshots, const pressure::Sample& sample,
                       std::optional<steve::Status> steve,
                       std::vector<emerge::Pending> merge_list) {
    if (!watched_) {
        return;
    }
    auto& watched = *watched_;
    watched.snapshots = std::move(snapshots);
    watched.steve = std::move(steve);
    watched.merge_list = std::move(merge_list);
    if (watched.merge_list.empty()) {
        watched.waits.clear();
        watched.planned.clear();
        watched.plan_error.reset();
    }
    if (!watched.steve || !watched.steve->live) {
        watched.editing.reset();
    }
    if (sample.cpu || sample.mem_available || sample.load) {
        watched.history.add(sample);
    }
    watched.due = false;
    watched.urgent = false;
    ++watched.frame;
    const auto count = watch_rows(watched).size();
    watched.cursor.at = std::min(watched.cursor.at, count == 0 ? 0 : count - 1);
}

namespace {

std::vector<std::string> sorted_cpvs(const std::vector<emerge::Pending>& list) {
    std::vector<std::string> cpvs;
    cpvs.reserve(list.size());
    for (const auto& pending : list) {
        cpvs.push_back(pending.cpv);
    }
    std::ranges::sort(cpvs);
    return cpvs;
}

} // namespace

std::optional<std::vector<emerge::Pending>> App::plan_requested() const {
    if (!watched_ || !pages_.empty()) {
        return std::nullopt;
    }
    const auto& watched = *watched_;
    const bool unknown = std::ranges::any_of(watched.merge_list, [&](const auto& pending) {
        return !watched.waits.contains(pending.cpv);
    });
    // Asked for this very list already: it failed, and asking again will not help.
    if (!unknown || sorted_cpvs(watched.merge_list) == watched.planned) {
        return std::nullopt;
    }
    return watched.merge_list;
}

void App::finish_plan(std::expected<emerge::Waits, std::string> result) {
    if (!watched_) {
        return;
    }
    auto& watched = *watched_;
    watched.planned = sorted_cpvs(watched.merge_list);
    if (!result) {
        watched.plan_error = std::move(result.error());
        return;
    }
    watched.plan_error.reset();
    for (auto& [cpv, waits] : *result) {
        watched.waits.insert_or_assign(cpv, std::move(waits));
    }
}

std::optional<SteveChange> App::steve_change_requested() const {
    if (!watched_ || !pages_.empty()) {
        return std::nullopt;
    }
    return watched_->change;
}

void App::finish_steve_change(const std::expected<void, std::string>& result) {
    if (!watched_) {
        return;
    }
    watched_->change.reset();
    // Whatever steve made of it, show it.
    watched_->due = true;
    watched_->urgent = true;
    if (!result) {
        show(
            {.error = true, .title = "stevie could not change it", .lines = lines(result.error())});
    }
}

void App::handle_steve(const Key& key) {
    if (!watched_ || !watched_->editing) {
        return;
    }
    auto& watched = *watched_;
    auto& at = *watched.editing;
    int direction = 0;
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace || is(key, U's')) {
        watched.editing.reset();
    } else if (key.kind == KeyKind::left || is(key, U'h')) {
        at = at > 0 ? at - 1 : at;
    } else if (key.kind == KeyKind::right || is(key, U'l')) {
        at = std::min(at + 1, steve::all_settings.size() - 1);
    } else if (key.kind == KeyKind::up || is(key, U'k') || is(key, U'+') || is(key, U'=')) {
        direction = 1;
    } else if (key.kind == KeyKind::down || is(key, U'j') || is(key, U'-')) {
        direction = -1;
    }
    if (direction == 0 || !watched.steve || !watched.steve->live) {
        return;
    }
    const auto& latest = watched.history.latest();
    const auto setting = steve::all_settings.at(at);
    if (const auto value =
            steve::step(setting, watched.steve->settings, direction, latest ? latest->cpus : 0)) {
        watched.change = SteveChange{.setting = setting, .value = *value};
    }
}

std::optional<std::chrono::milliseconds> App::refresh() const {
    if (check_requested() || rebuild_requested() || refresh_requested() || index_requested() ||
        running_ || planning_ || trying_) {
        return wait_interval;
    }
    if (watched_ && pages_.empty()) {
        return watch_interval;
    }
    return std::nullopt;
}

void App::handle_watch(const Key& key) {
    if (!watched_) {
        return;
    }
    auto& watched = *watched_;
    if (watched.editing) {
        handle_steve(key);
        return;
    }
    const auto all = watch_rows(watched);
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (is(key, U's')) {
        if (!watched.steve) {
            show({.error = false,
                  .title = "steve is not running",
                  .lines = {"It shares one pool of jobs between every build when it runs."}});
        } else if (!watched.steve->live) {
            show({.error = false,
                  .title = "steve's settings can only be read here",
                  .lines = {watched.steve->problem,
                            "stevie needs /dev/steve: root, or the jobserver group."}});
        } else {
            watched.editing = 0;
        }
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        watched_.reset();
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (watched.cursor.at >= all.size()) {
            return;
        }
        // What is building is not installed yet; its page is the installed version's.
        const auto parts = split_cpv(all.at(watched.cursor.at).cpv);
        const auto cp = std::format("{}/{}", parts.category, parts.name);
        const auto installed = resolve(store(), cp);
        if (installed && !installed->empty()) {
            open(installed->back());
        } else {
            show({.error = false,
                  .title = std::format("No version of {} is installed yet", cp),
                  .lines = {"Pages show installed packages, and it has none so far."}});
        }
    } else if (is_move(key)) {
        move(watched.cursor, all.size(), key, height_);
    }
}

std::optional<std::uint32_t> App::find(std::string_view cpv) const {
    // Packages are in cpv order.
    const auto& packages = store().packages;
    const auto cpv_of = [this](const Package& pkg) { return store().string(pkg.cpv); };
    const auto found = std::ranges::lower_bound(packages, cpv, {}, cpv_of);
    if (found == packages.end() || cpv_of(*found) != cpv) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::distance(packages.begin(), found));
}

void App::handle_check(const Key& key) {
    if (!checked_) {
        return;
    }
    auto& checked = *checked_;
    const bool waiting = check_requested() || rebuild_requested();
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        // Leaving stops a build still running.
        checked_.reset();
    } else if (waiting) {
        return;
    } else if (is(key, U'r')) {
        checked = {};
    } else if (is(key, U'u')) {
        if (checked.stage != Checked::Stage::checked || checked.drift.empty()) {
            return;
        }
        if (update_ == Update::save) {
            checked.stage = Checked::Stage::rebuilding;
        } else if (checked.fresh) {
            auto fresh = std::move(*checked.fresh);
            checked.fresh.reset();
            checked.drift.clear();
            checked.stage = Checked::Stage::rebuilt;
            checked.cursor = {};
            adopt(std::make_shared<const Stores>(std::move(fresh)), Source::preview);
        }
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (checked.cursor.at < checked.drift.size()) {
            const auto cpv = std::string_view{checked.drift.at(checked.cursor.at)}.substr(1);
            if (const auto id = find(cpv)) {
                open(*id);
            } else {
                show({.error = false,
                      .title = std::format("{} is not in the store", cpv),
                      .lines = {"It was installed after the store was built.",
                                update_ == Update::save
                                    ? "u rebuilds the store to include it."
                                    : "u shows the fresh build, which has it."}});
            }
        }
    } else if (is_move(key)) {
        move(checked.cursor, checked.drift.size(), key, height_);
    }
}

void App::handle_list(const Key& key) {
    if (list_.searching) {
        if (key.kind == KeyKind::character && key.code >= U' ') {
            append_utf8(list_.query, key.code);
            filter();
        } else if (key.kind == KeyKind::backspace) {
            pop_code_point(list_.query);
            filter();
        } else if (key.kind == KeyKind::enter) {
            list_.searching = false;
        } else if (key.kind == KeyKind::escape) {
            list_.searching = false;
            list_.query.clear();
            filter();
        } else if (is_move(key) && key.kind != KeyKind::character) {
            move(list_.cursor, list_.shown.size(), key, height_);
        }
        return;
    }
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (is(key, U'/')) {
        list_.searching = true;
    } else if (is(key, U's')) {
        search_ = Search{};
        index_failed_ = false;
    } else if (is(key, U'o')) {
        list_.only = list_.only == Only::orphans ? Only::all : Only::orphans;
        filter();
    } else if (is(key, U'!')) {
        list_.only = list_.only == Only::broken ? Only::all : Only::broken;
        filter();
    } else if (is(key, U'u') && has_evaluated()) {
        list_.only = list_.only == Only::updates ? Only::all : Only::updates;
        filter();
    } else if (is(key, U'c')) {
        checked_.emplace();
    } else if (is(key, U'e')) {
        watched_.emplace();
    } else if (is(key, U'p') && has_evaluated() && scope_planned(scope_)) {
        open_plan();
    } else if (key.kind == KeyKind::left || is(key, U'h')) {
        turn_page(-1);
    } else if (key.kind == KeyKind::right || is(key, U'l')) {
        turn_page(1);
    } else if (is(key, U'b')) {
        build_deps_ = !build_deps_;
        recompute();
        filter();
    } else if (is(key, U' ')) {
        if (list_.cursor.at < list_.shown.size()) {
            const auto& store = this->store();
            std::string cpv{store.string(store.packages.at(list_.shown.at(list_.cursor.at)).cpv)};
            if (const auto at = std::ranges::find(picked_, cpv); at != picked_.end()) {
                picked_.erase(at);
            } else {
                picked_.push_back(std::move(cpv));
            }
            move(list_.cursor, list_.shown.size(), {.kind = KeyKind::down, .code = 0}, height_);
        }
    } else if (is(key, U'U') && has_evaluated()) {
        Action action{
            .kind = Action::Kind::update, .targets = {}, .scope = scope_, .build_deps = true};
        for (const auto& cpv : picked_) {
            if (const auto id = find(cpv)) {
                const auto& pkg = store().packages.at(*id);
                action.targets.push_back(
                    std::format("{}:{}", store().string(pkg.cp), store().string(pkg.slot)));
            }
        }
        act(std::move(action));
    } else if (is(key, U'r')) {
        Action action{.kind = Action::Kind::remove, .targets = {}, .build_deps = build_deps_};
        if (picked_.empty() && list_.only == Only::orphans) {
            for (const auto id : list_.shown) {
                action.targets.push_back(
                    std::format("={}", store().string(store().packages.at(id).cpv)));
            }
        }
        for (const auto& cpv : picked_) {
            action.targets.push_back(std::format("={}", cpv));
        }
        if (action.targets.empty()) {
            show({.error = false,
                  .title = "Nothing picked to remove",
                  .lines = {
                      "Pick packages with space, or show the orphans with o to remove them all."}});
        } else {
            act(std::move(action));
        }
    } else if (key.kind == KeyKind::escape && !list_.query.empty()) {
        list_.query.clear();
        filter();
    } else if (key.kind == KeyKind::enter) {
        if (list_.cursor.at < list_.shown.size()) {
            open(list_.shown.at(list_.cursor.at));
        }
    } else if (is_move(key)) {
        move(list_.cursor, list_.shown.size(), key, height_);
    }
}

std::optional<TriedMerge> App::tried_of(std::string_view first) const {
    const auto& tried = current().tried;
    if (const auto found = std::ranges::find(tried, first, &TriedMerge::first);
        found != tried.end()) {
        return *found;
    }
    return std::nullopt;
}

Planned& App::open_plan(std::optional<std::string> selected) {
    Planned planned{.rows = plan_rows(store(), evaluated(), kept_, plan()),
                    .places = std::vector<std::size_t>(plan().merges.size(), 0),
                    .cursor = {}};
    // Last, what the lines tried drop.
    const auto& tried = current().tried;
    for (std::size_t i = 0; i < tried.size(); ++i) {
        if (tried.at(i).change != TriedMerge::Change::dropped) {
            continue;
        }
        if (!planned.rows.empty() && planned.rows.back().dropped) {
            planned.rows.back().last = false;
        } else {
            planned.rows.push_back({.label = {},
                                    .merge = std::nullopt,
                                    .dropped = i,
                                    .depth = 0,
                                    .last = false,
                                    .rails = {}});
        }
        planned.rows.push_back({.label = tried.at(i).first,
                                .merge = std::nullopt,
                                .dropped = i,
                                .depth = 1,
                                .last = true,
                                .rails = {}});
    }
    for (std::size_t place = 0; place < plan().order.size(); ++place) {
        planned.places.at(plan().order.at(place)) = place + 1;
    }
    const auto& rows = planned.rows;
    auto at = std::ranges::find_if(
        rows, [&](const PlanRow& row) { return row.depth > 0 && row.label == selected; });
    if (at == rows.end()) {
        at = std::ranges::find_if(rows, [](const PlanRow& row) { return row.depth > 0; });
    }
    planned.cursor.at = at == rows.end() ? 0 : static_cast<std::size_t>(at - rows.begin());
    keep_visible(planned.cursor, height_);
    if (planned.cursor.at < height_) {
        planned.cursor.top = 0;
    }
    return planned_.emplace(std::move(planned));
}

void App::handle_plan(const Key& key) {
    if (!planned_) {
        return;
    }
    auto& planned = *planned_;
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        planned_.reset();
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (planned.cursor.at >= planned.rows.size()) {
            return;
        }
        const auto& row = planned.rows.at(planned.cursor.at);
        if (row.depth == 0) {
            return;
        }
        if (const auto id = find(row.label)) {
            open(*id);
            return;
        }
        std::vector<std::string> lines;
        if (row.merge) {
            const auto& merge = plan().merges.at(*row.merge);
            if (const auto& by = merge.pulled_by) {
                lines.push_back(std::format("The plan pulls it in for {}'s {}.",
                                            member_cpv(store(), evaluated(), by->member),
                                            by->atom));
            } else if (const auto& named = merge.named_by) {
                lines.push_back(std::format(
                    "The plan pulls it in for {}.",
                    named->set.empty() ? named->atom
                                       : std::format("@{}'s {}", named->set, named->atom)));
            }
            if (const auto use =
                    use_display(evaluated(), evaluated().candidates.at(merge.candidate));
                !use.empty()) {
                lines.push_back(std::format("It would be built with {}.", use));
            }
        }
        show({.error = false,
              .title = std::format("{} is not installed", row.label),
              .lines = std::move(lines)});
    } else if (is_move(key)) {
        std::vector<std::size_t> stops;
        for (std::size_t i = 0; i < planned.rows.size(); ++i) {
            if (planned.rows.at(i).depth > 0) {
                stops.push_back(i);
            }
        }
        move_among(planned.cursor, stops, key, height_);
    }
}

void App::finish_index(IndexResult result) {
    if (!result) {
        index_failed_ = true;
        show({.error = true,
              .title = "The repository index could not be loaded",
              .lines = lines(result.error())});
        return;
    }
    catalogue_.reset();
    indexed_ = std::make_unique<const Indexed>(std::move(*result));
    catalogue_.emplace(installed(), evaluated(), *indexed_->index, indexed_->masks);
    if (search_ && search_->pending) {
        run_search();
    }
}

void App::run_search() {
    if (!search_) {
        return;
    }
    auto& search = *search_;
    search.ran.reset();
    search.results.clear();
    search.cursor = {};
    search.pending = !catalogue_;
    if (!catalogue_) {
        return;
    }
    const auto found = catalogue_->search(search.query, {.description = search.descriptions});
    if (!found) {
        show({.error = true, .title = "The search could not run", .lines = {found.error()}});
        return;
    }
    search.ran = search.query;
    search.results.reserve(found->size());
    for (const auto cp : *found) {
        search.results.push_back(catalogue_->found(cp));
    }
}

std::vector<Row> App::version_rows(std::string_view cp) const {
    if (!catalogue_ || !catalogue_->contains(cp)) {
        return {};
    }
    const auto versions = catalogue_->versions(cp);
    std::vector<Row> rows{text_row(RowType::heading, std::format("Versions  {}", versions.size()))};
    for (const auto& version : versions) {
        auto row = text_row(RowType::version, "");
        row.version = version;
        rows.push_back(std::move(row));
    }
    return rows;
}

namespace {

std::vector<std::size_t> version_stops(const std::vector<Row>& rows) {
    std::vector<std::size_t> stops;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows.at(i).type == RowType::version) {
            stops.push_back(i);
        }
    }
    return stops;
}

} // namespace

void App::open_listing(const Found& found) {
    Listing listing{.found = found, .rows = {}, .cursor = {}};
    auto& rows = listing.rows;
    for (const auto& [label, text] :
         {std::pair{"", found.description}, std::pair{"", found.homepage},
          std::pair{"License: ", found.license}}) {
        if (!text.empty()) {
            rows.push_back(text_row(RowType::note, std::format("{}{}", label, text)));
        }
    }
    if (!rows.empty()) {
        rows.push_back(text_row(RowType::note, ""));
    }
    std::ranges::move(version_rows(found.cp), std::back_inserter(rows));
    // On the version a search shows, else the first.
    const auto stops = version_stops(rows);
    const auto best = std::ranges::find_if(stops, [&](std::size_t at) {
        const auto& version = rows.at(at).version;
        return version && version->ebuild &&
               (version->version == found.version || version->version == found.version + "-r0");
    });
    listing.cursor.at = best != stops.end() ? *best : stops.empty() ? 0 : stops.front();
    keep_visible(listing.cursor, height_);
    listing_ = std::move(listing);
}

void App::handle_search(const Key& key) {
    if (!search_) {
        return;
    }
    auto& search = *search_;
    if (key.kind == KeyKind::tab) {
        search.descriptions = !search.descriptions;
        if (search.ran || search.pending) {
            run_search();
        }
        return;
    }
    if (search.typing) {
        if (key.kind == KeyKind::character && key.code >= U' ') {
            append_utf8(search.query, key.code);
        } else if (key.kind == KeyKind::backspace) {
            pop_code_point(search.query);
        } else if (key.kind == KeyKind::enter) {
            search.typing = false;
            // An empty key would list every package.
            if (!search.query.empty()) {
                run_search();
            }
        } else if (key.kind == KeyKind::escape) {
            search.typing = false;
            if (!search.ran && !search.pending) {
                search_.reset();
            }
        }
        return;
    }
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        search_.reset();
    } else if (is(key, U'/') || is(key, U's')) {
        search.typing = true;
    } else if (is(key, U'i')) {
        if (search.cursor.at < search.results.size()) {
            act({.kind = Action::Kind::install,
                 .targets = {search.results.at(search.cursor.at).cp},
                 .build_deps = true});
        }
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (search.cursor.at >= search.results.size()) {
            return;
        }
        const auto found = search.results.at(search.cursor.at);
        if (found.installed.empty()) {
            open_listing(found);
            return;
        }
        if (const auto id = find(std::format("{}-{}", found.cp, found.installed))) {
            open(*id);
        } else if (const auto installed = resolve(store(), found.cp);
                   installed && !installed->empty()) {
            open(installed->back());
        }
    } else if (is_move(key)) {
        move(search.cursor, search.results.size(), key, height_);
    }
}

void App::handle_listing(const Key& key) {
    if (!listing_) {
        return;
    }
    auto& listing = *listing_;
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        listing_.reset();
    } else if (is(key, U'i')) {
        if (listing.cursor.at < listing.rows.size()) {
            if (const auto& version = listing.rows.at(listing.cursor.at).version) {
                install(listing.found.cp, *version);
            }
        }
    } else if (is_move(key)) {
        move_among(listing.cursor, version_stops(listing.rows), key, height_);
    }
}

bool App::can_unfold(const Row& row) const {
    if (row.type != RowType::link || row.cycle) {
        return false;
    }
    return (row.reverse ? dependents(row.link.package) : dependencies(row.link.package)) > 0;
}

void App::unfold(Page& page) {
    const auto at = page.cursor.at;
    const auto& row = page.rows.at(at);
    if (row.unfolded || !can_unfold(row)) {
        return;
    }
    std::vector<std::uint32_t> path{page.package, row.link.package};
    for (auto up = parent_of(page.rows, at); up; up = parent_of(page.rows, *up)) {
        path.push_back(page.rows.at(*up).link.package);
    }
    const auto depth = row.depth + 1;
    const auto reverse = row.reverse;
    std::vector<Row> children;
    for (const auto& link : links(store(), graph_.get(), row.link.package, reverse)) {
        children.push_back(link_row(link, depth, reverse));
        children.back().cycle = std::ranges::contains(path, link.package);
    }
    page.rows.at(at).unfolded = true;
    const auto count = children.size();
    page.rows.insert(page.rows.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                     std::make_move_iterator(children.begin()),
                     std::make_move_iterator(children.end()));
    thread(page.rows);
    // Bring the unfolded rows into view, as far as the cursor stays on screen.
    if (at + count >= page.cursor.top + height_) {
        page.cursor.top = std::min(at, at + count + 1 - height_);
    }
}

void App::fold(Page& page) {
    const auto at = page.cursor.at;
    const auto depth = page.rows.at(at).depth;
    auto end = at + 1;
    while (end < page.rows.size() && page.rows.at(end).type == RowType::link &&
           page.rows.at(end).depth > depth) {
        ++end;
    }
    page.rows.erase(page.rows.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                    page.rows.begin() + static_cast<std::ptrdiff_t>(end));
    page.rows.at(at).unfolded = false;
    thread(page.rows);
}

void App::handle_page(const Key& key) {
    auto& page = pages_.back();
    const bool on_link =
        page.cursor.at < page.rows.size() && selectable(page.rows.at(page.cursor.at));
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace) {
        pages_.pop_back();
    } else if (!on_link) {
        // On a version: another installed one opens its page, and i installs it.
        std::optional<PackageVersion> version;
        if (page.cursor.at < page.rows.size()) {
            version = page.rows.at(page.cursor.at).version;
        }
        const auto installed = version ? version->installed : std::nullopt;
        if (key.kind == KeyKind::left || is(key, U'h')) {
            pages_.pop_back();
        } else if (key.kind == KeyKind::enter && installed && *installed != page.package) {
            open(*installed);
        } else if (is(key, U'i') && version) {
            install(store().string(store().packages.at(page.package).cp), *version);
        } else if (is(key, U'a') || is(key, U'e')) {
            typed_line_ = TypedLine{
                .file = is(key, U'a') ? WhatIfLine::File::use : WhatIfLine::File::env,
                .text = std::format("{} ", store().string(store().packages.at(page.package).cp))};
        } else if (const auto& flagged = page.cursor.at < page.rows.size()
                                             ? page.rows.at(page.cursor.at).flag
                                             : std::nullopt;
                   (is(key, U' ') || is(key, U'*')) && flagged) {
            const auto flag = *flagged;
            if (flag.fixed) {
                show({.error = true,
                      .title = std::format("{} is fixed", flag.flag),
                      .lines = {"The profile forces or masks it (use.force, use.mask), which "
                                "package.use cannot change."}});
            } else {
                toggle(flag,
                       is(key, U'*')
                           ? std::string{"*/*"}
                           : std::string{store().string(store().packages.at(page.package).cp)});
            }
        } else if (is_move(key)) {
            move_on_page(page, key, height_);
        }
    } else if (key.kind == KeyKind::enter) {
        if (const auto target = page.rows.at(page.cursor.at).link.package; target != page.package) {
            open(target);
        }
    } else if (is(key, U' ') || key.kind == KeyKind::tab) {
        if (page.rows.at(page.cursor.at).unfolded) {
            fold(page);
        } else {
            unfold(page);
        }
    } else if (key.kind == KeyKind::right || is(key, U'l')) {
        // Unfolds, or steps onto the first child of an unfolded link.
        if (page.rows.at(page.cursor.at).unfolded) {
            ++page.cursor.at;
            keep_visible(page.cursor, height_);
        } else {
            unfold(page);
        }
    } else if (key.kind == KeyKind::left || is(key, U'h')) {
        // Folds, steps up to the parent link, or at the top goes back.
        if (page.rows.at(page.cursor.at).unfolded) {
            fold(page);
        } else if (const auto up = parent_of(page.rows, page.cursor.at)) {
            page.cursor.at = *up;
            keep_visible(page.cursor, height_);
        } else {
            pages_.pop_back();
        }
    } else if (is_move(key)) {
        move_on_page(page, key, height_);
    }
}

std::size_t columns(std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return !is_continuation(c); }));
}

std::string duration(double seconds) {
    const auto whole = seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
    if (whole < 60) {
        return std::format("{}s", whole);
    }
    if (whole < 3600) {
        return std::format("{}m{:02}s", whole / 60, whole % 60);
    }
    return std::format("{}h{:02}m", whole / 3600, whole % 3600 / 60);
}

std::string byte_size(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    constexpr std::array<std::string_view, 5> units{"KiB", "MiB", "GiB", "TiB", "PiB"};
    auto value = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < units.size()) {
        value /= 1024;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units.at(unit));
}

std::string progress_bar(std::uint64_t done, std::uint64_t total, std::size_t width,
                         const Glyphs& glyph) {
    const auto eighths = columns(glyph.bar_eighths);
    // In eighths of a cell, whether or not the glyphs can show them.
    const auto filled = total == 0 ? 0 : std::min(done, total) * width * 8 / total;
    const auto full = static_cast<std::size_t>(filled / 8);
    const auto part = static_cast<std::size_t>(filled % 8);
    std::string bar = repeat(glyph.bar_full, full);
    std::size_t used = full;
    if (part > 0 && eighths >= 7 && used < width) {
        bar += code_points(glyph.bar_eighths, part - 1, 1);
        ++used;
    }
    return bar + repeat(glyph.bar_empty, width - used);
}

bool previewed(const Action& action) {
    return action.kind != Action::Kind::sync;
}

std::optional<Action> notice_action(const Notice& notice) {
    const auto named = notice.key.substr(notice.key.find(':') + 1);
    switch (notice.kind) {
    case NoticeKind::glsa: {
        Action update{.kind = Action::Kind::update};
        for (const auto& cpv : notice.packages) {
            const auto parts = split_cpv(cpv);
            auto cp = std::format("{}/{}", parts.category, parts.name);
            if (!std::ranges::contains(update.targets, cp)) {
                update.targets.push_back(std::move(cp));
            }
        }
        if (update.targets.empty()) {
            return std::nullopt;
        }
        return update;
    }
    case NoticeKind::missing:
        if (notice.packages.empty()) {
            return std::nullopt;
        }
        return Action{.kind = Action::Kind::rebuild, .targets = {"=" + notice.packages.front()}};
    case NoticeKind::preserved:
        return Action{.kind = Action::Kind::rebuild, .targets = {"@preserved-rebuild"}};
    case NoticeKind::stale:
        return Action{.kind = Action::Kind::sync, .targets = {named}};
    case NoticeKind::plan:
        return Action{.kind = Action::Kind::update, .targets = {}, .scope = Scope::world};
    case NoticeKind::masked:
    case NoticeKind::news:
    case NoticeKind::config:
    case NoticeKind::check:
        break;
    }
    return std::nullopt;
}

std::string_view notice_work(const Notice& notice) {
    if (notice.kind == NoticeKind::masked) {
        return notice.packages.empty() ? "" : "open";
    }
    if (notice.kind == NoticeKind::news) {
        return notice.file.empty() ? "" : "read";
    }
    if (notice.kind == NoticeKind::config) {
        return "dispatch-conf";
    }
    if (notice.kind == NoticeKind::check) {
        return "check";
    }
    const auto action = notice_action(notice);
    if (!action) {
        return {};
    }
    switch (action->kind) {
    case Action::Kind::update:
        return "update";
    case Action::Kind::rebuild:
        return "rebuild";
    case Action::Kind::sync:
        return "sync";
    case Action::Kind::install:
    case Action::Kind::remove:
        break;
    }
    return {};
}

std::vector<std::string> action_arguments(const Action& action) {
    std::vector<std::string> words;
    switch (action.kind) {
    case Action::Kind::install:
        words = {"exec"};
        break;
    case Action::Kind::update:
        words = {"exec", "--oneshot", "-u", "-N"};
        if (action.targets.empty()) {
            words.insert(words.end(), {"-D", std::string{scope_name(action.scope)}});
        }
        break;
    case Action::Kind::rebuild:
        words = {"exec", "--oneshot"};
        break;
    case Action::Kind::remove:
        words = {"remove"};
        if (!action.build_deps) {
            words.insert(words.end(), {"--with-bdeps", "n"});
        }
        break;
    case Action::Kind::sync:
        words = {"sync"};
        break;
    }
    words.insert(words.end(), action.targets.begin(), action.targets.end());
    return words;
}

std::vector<std::string> plain_tail(std::string_view text, std::size_t count) {
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto c = text.at(at);
        if (c == '\x1b') {
            if (at + 1 < text.size() && text.at(at + 1) == '[') {
                // A control sequence ends at its final byte.
                at += 2;
                while (at < text.size() && (text.at(at) < '@' || text.at(at) > '~')) {
                    ++at;
                }
            } else if (at + 1 < text.size() && text.at(at + 1) == ']') {
                // An operating system command ends at BEL or ST.
                at += 2;
                while (
                    at < text.size() && text.at(at) != '\a' &&
                    !(text.at(at) == '\x1b' && at + 1 < text.size() && text.at(at + 1) == '\\')) {
                    ++at;
                }
                if (at < text.size() && text.at(at) == '\x1b') {
                    ++at;
                }
            } else {
                ++at;
            }
        } else if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else if (c == '\r') {
            if (at + 1 >= text.size() || text.at(at + 1) != '\n') {
                line.clear();
            }
        } else if (c == '\t') {
            line.append(8 - (columns(line) % 8), ' ');
        } else if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f') {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().find_first_not_of(' ') == std::string::npos) {
        lines.pop_back();
    }
    if (lines.size() > count) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(count));
    }
    return lines;
}

std::string spinner_frame(std::size_t count, const Glyphs& glyph) {
    const auto frames = columns(glyph.spinner);
    return frames == 0 ? std::string{} : code_points(glyph.spinner, count % frames, 1);
}

std::vector<Span> sparkline(const std::vector<std::optional<double>>& values, double max,
                            std::size_t width, const Glyphs& glyph, Tone tone,
                            std::optional<double> limit) {
    const auto levels = columns(glyph.spark);
    std::vector<Span> spans;
    // Each cell joins the span before it when it is drawn in the same tone.
    std::optional<Tone> last_tone;
    const auto add = [&](std::string cell, Tone cell_tone) {
        if (last_tone == cell_tone) {
            spans.back().text += cell;
        } else {
            spans.push_back({std::move(cell), tone_pen(cell_tone)});
            last_tone = cell_tone;
        }
    };
    const auto shown = std::min(values.size(), width);
    for (std::size_t i = shown; i < width; ++i) {
        add(" ", tone);
    }
    for (std::size_t i = values.size() - shown; i < values.size(); ++i) {
        const auto& value = values.at(i);
        if (!value || levels == 0) {
            add(" ", tone);
            continue;
        }
        const auto scaled = max > 0 ? std::clamp(*value / max, 0.0, 1.0) : 0.0;
        const auto level =
            static_cast<std::size_t>(std::lround(scaled * static_cast<double>(levels - 1)));
        add(code_points(glyph.spark, level, 1), limit && *value > *limit ? Tone::bad : tone);
    }
    return spans;
}

std::vector<std::vector<Span>> pressure_lines(const pressure::History& history,
                                              const Limits& limits, std::size_t width,
                                              const Glyphs& glyph) {
    const auto& readings = history.readings();
    const auto& latest = history.latest();
    const auto series = [&](auto value) {
        std::vector<std::optional<double>> values;
        values.reserve(readings.size());
        for (const auto& reading : readings) {
            values.push_back(value(reading));
        }
        return values;
    };
    const auto label = [](std::string_view name) {
        return Span{std::format(" {:<9}", name), tone_pen(Tone::heading)};
    };
    const auto note = [](std::string text) { return Span{std::move(text), tone_pen(Tone::note)}; };
    const auto last = [](const std::vector<std::optional<double>>& values) {
        return values.empty() ? std::nullopt : values.back();
    };
    std::vector<std::vector<Span>> lines{{{" System", tone_pen(Tone::heading)}}};

    const auto cpu = series([](const pressure::Reading& r) {
        return r.cpu_busy ? std::optional<double>{*r.cpu_busy * 100} : std::nullopt;
    });
    auto line = std::vector<Span>{label("CPU")};
    std::ranges::move(sparkline(cpu, 100, width, glyph, Tone::choice), std::back_inserter(line));
    const auto cpu_now = last(cpu);
    line.push_back(
        {cpu_now ? std::format("  {:>5.0f}%", *cpu_now) : "      -", tone_pen(Tone::version)});
    if (latest && latest->cpus > 0) {
        line.push_back(note(std::format("   {} CPUs", latest->cpus)));
    }
    lines.push_back(std::move(line));

    // Memory in use, so that higher is harder pressed like the rest.
    const auto total = latest && latest->mem_total ? static_cast<double>(*latest->mem_total) : 0.0;
    const auto used = series([&](const pressure::Reading& r) {
        return r.mem_available && total > 0
                   ? std::optional<double>{total - static_cast<double>(*r.mem_available)}
                   : std::nullopt;
    });
    const auto used_limit =
        limits.min_available && total > 0
            ? std::optional<double>{total - static_cast<double>(*limits.min_available)}
            : std::nullopt;
    line = {label("Memory")};
    std::ranges::move(sparkline(used, total, width, glyph, Tone::host, used_limit),
                      std::back_inserter(line));
    if (latest && latest->mem_available && latest->mem_total) {
        line.push_back({std::format("  {} available of {}", byte_size(*latest->mem_available),
                                    byte_size(*latest->mem_total)),
                        tone_pen(Tone::version)});
    }
    if (limits.min_available) {
        line.push_back(
            note(std::format("   steve waits under {}", byte_size(*limits.min_available))));
    }
    lines.push_back(std::move(line));

    const auto load = series([](const pressure::Reading& r) { return r.load; });
    auto load_max =
        std::max(latest ? static_cast<double>(latest->cpus) : 0.0, limits.load.value_or(0.0));
    for (const auto& value : load) {
        load_max = std::max(load_max, value.value_or(0.0));
    }
    line = {label("Load")};
    std::ranges::move(sparkline(load, load_max, width, glyph, Tone::runtime, limits.load),
                      std::back_inserter(line));
    const auto load_now = last(load);
    line.push_back(
        {load_now ? std::format("  {:>6.2f}", *load_now) : "       -", tone_pen(Tone::version)});
    if (limits.load) {
        line.push_back(note(std::format("   steve waits over {:g}", *limits.load)));
    }
    lines.push_back(std::move(line));

    line = {label("Stalls")};
    const auto stalls = latest ? latest->stalls : pressure::Stalls{};
    if (stalls.cpu || stalls.memory || stalls.io) {
        const auto percent = [](std::optional<double> value) {
            return value ? std::format("{:.1f}%", *value) : std::string{"-"};
        };
        line.push_back({std::format("cpu {}  memory {}  io {}", percent(stalls.cpu),
                                    percent(stalls.memory), percent(stalls.io)),
                        tone_pen(Tone::version)});
        line.push_back(note("   of the last 10 s"));
    } else {
        line.push_back(note("not kept by this kernel; psi=1 on its command line turns them on"));
    }
    lines.push_back(std::move(line));
    return lines;
}

std::size_t rows_owner(const Watched& watched) {
    std::set<std::string_view, std::less<>> listed;
    for (const auto& pending : watched.merge_list) {
        listed.insert(pending.cpv);
    }
    std::size_t owner = watched.snapshots.size();
    std::size_t most = 0;
    std::size_t emerges = 0;
    for (std::size_t at = 0; at < watched.snapshots.size(); ++at) {
        if (watched.snapshots.at(at).publisher != emerge::Publisher::emerge) {
            continue;
        }
        ++emerges;
        const auto& tasks = watched.snapshots.at(at).tasks;
        const auto count = static_cast<std::size_t>(std::ranges::count_if(
            tasks, [&](const emerge::Task& task) { return listed.contains(task.cpv); }));
        if (count > most) {
            owner = at;
            most = count;
        }
    }
    // One emerge between tasks still owns the list it left.
    if (owner == watched.snapshots.size() && emerges == 1 && !listed.empty()) {
        for (std::size_t at = 0; at < watched.snapshots.size(); ++at) {
            if (watched.snapshots.at(at).publisher == emerge::Publisher::emerge) {
                owner = at;
            }
        }
    }
    return owner;
}

std::vector<WatchRow> watch_rows(const Watched& watched) {
    const auto owner = rows_owner(watched);
    std::vector<WatchRow> all;
    for (std::size_t at = 0; at < watched.snapshots.size(); ++at) {
        const auto& tasks = watched.snapshots.at(at).tasks;
        std::vector<WatchRow> rows;
        const auto listed = [&](const emerge::Task& task) {
            return at == owner && std::ranges::any_of(watched.merge_list, [&](const auto& pending) {
                       return pending.cpv == task.cpv;
                   });
        };
        for (const auto& task : tasks) {
            if (!listed(task)) {
                rows.push_back({.snapshot = at, .cpv = task.cpv, .task = task});
            }
        }
        if (at == owner) {
            for (const auto& branch : emerge::hierarchy(watched.merge_list, watched.waits)) {
                const auto task = std::ranges::find(tasks, branch.cpv, &emerge::Task::cpv);
                const auto pending =
                    std::ranges::find(watched.merge_list, branch.cpv, &emerge::Pending::cpv);
                rows.push_back(
                    {.snapshot = at,
                     .cpv = branch.cpv,
                     .task = task == tasks.end() ? std::nullopt : std::optional{*task},
                     .waiting_for = branch.waiting_for,
                     .binary = pending != watched.merge_list.end() && pending->kind == "binary",
                     .depth = branch.depth});
            }
        }
        thread_tree(rows, [](const WatchRow& row) { return row.depth; });
        std::ranges::move(rows, std::back_inserter(all));
    }
    return all;
}

Limits limits_of(const std::optional<steve::Status>& steve) {
    if (!steve) {
        return {};
    }
    const auto& settings = steve->settings;
    return {.load = settings.load_average,
            .min_available =
                settings.min_memory && *settings.min_memory > 0
                    ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*settings.min_memory)
                                                   << 20U}
                    : std::nullopt};
}

std::vector<Span> steve_line(const std::optional<steve::Status>& steve,
                             std::optional<std::size_t> editing, const Glyphs& glyph) {
    std::vector<Span> spans{{std::format(" {:<9}", "Steve"), tone_pen(Tone::heading)}};
    if (!steve) {
        spans.push_back({"not running", tone_pen(Tone::note)});
        return spans;
    }
    const auto& settings = steve->settings;
    if (steve->live && settings.tokens && settings.jobs && *settings.jobs > 0) {
        const auto jobs = static_cast<std::uint64_t>(*settings.jobs);
        const auto used = static_cast<std::uint64_t>(
            std::clamp<std::int64_t>(*settings.jobs - *settings.tokens, 0, *settings.jobs));
        spans.push_back({progress_bar(used, jobs, 12, glyph), tone_pen(Tone::choice)});
        spans.push_back(
            {std::format("  {} of {} jobs in use   ", used, jobs), tone_pen(Tone::note)});
    }
    const auto whole = [](std::optional<std::int64_t> value) {
        return value ? std::format("{}", *value) : std::string{"-"};
    };
    const std::array<std::pair<std::string_view, std::string>, steve::all_settings.size()> items{{
        {"jobs", whole(settings.jobs)},
        {"min jobs", whole(settings.min_jobs)},
        {"load", settings.load_average ? std::format("{:g}", *settings.load_average) : "-"},
        {"memory",
         settings.min_memory
             ? byte_size(static_cast<std::uint64_t>(std::max<std::int64_t>(*settings.min_memory, 0))
                         << 20U)
             : "-"},
        {"per process", whole(settings.per_process)},
        {"recheck",
         settings.recheck_timeout ? std::format("{:g}s", *settings.recheck_timeout) : "-"},
    }};
    for (std::size_t at = 0; at < items.size(); ++at) {
        const auto& [name, value] = items.at(at);
        spans.push_back({std::format("{}{} ", at == 0 ? "" : "  ", name), tone_pen(Tone::note)});
        if (editing == at) {
            spans.push_back({std::format(" {} ", value),
                             {.fg = palette::crust, .bg = palette::mauve, .bold = true}});
        } else {
            spans.push_back({value, tone_pen(Tone::version)});
        }
    }
    if (!steve->live) {
        spans.push_back({steve->problem.empty()
                             ? std::string{"   from its command line"}
                             : std::format("   from its command line ({})", steve->problem),
                         tone_pen(Tone::note)});
    }
    return spans;
}

std::string task_state(const emerge::Task& task) {
    if (task.merge_wait) {
        return "waiting to merge";
    }
    if (!task.phase.empty()) {
        return task.phase;
    }
    return task.kind == emerge::TaskKind::merge ? "merging" : "starting";
}

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out += text;
    }
    return out;
}

std::string clip(std::string_view text, std::size_t width) {
    std::size_t kept = 0;
    std::size_t bytes = 0;
    while (bytes < text.size() && kept < width) {
        ++bytes;
        while (bytes < text.size() && is_continuation(text.at(bytes))) {
            ++bytes;
        }
        ++kept;
    }
    return std::string{text.substr(0, bytes)};
}

Pen tone_pen(Tone tone) {
    const auto style = tone_style(tone);
    return {.fg = Color{.red = style.red, .green = style.green, .blue = style.blue},
            .bg = std::nullopt,
            .bold = style.bold,
            .italic = style.italic};
}

std::vector<Span> cpv_spans(std::string_view cpv) {
    const auto parts = split_cpv(cpv);
    std::vector<Span> spans;
    if (!parts.category.empty()) {
        spans.push_back({std::string{parts.category}, tone_pen(Tone::category)});
        spans.push_back({"/", tone_pen(Tone::note)});
    }
    spans.push_back({std::string{parts.name}, tone_pen(Tone::name)});
    if (!parts.version.empty()) {
        spans.push_back({"-", tone_pen(Tone::note)});
        spans.push_back({std::string{parts.version}, tone_pen(Tone::version)});
    }
    return spans;
}

std::vector<Span> version_spans(const PackageVersion& version, const Glyphs& glyph) {
    const auto slot = version.sub_slot.empty() || version.sub_slot == version.slot
                          ? std::format(":{}", version.slot)
                          : std::format(":{}/{}", version.slot, version.sub_slot);
    std::vector<Span> spans{
        {version.installed ? std::string{glyph.good} : std::string{" "}, tone_pen(Tone::good)},
        {std::format(" {:<16}", version.version),
         tone_pen(version.visible ? Tone::version : Tone::bad)},
        {std::format("{:<12}", slot), tone_pen(Tone::note)},
        {std::format("{:<18}", std::format("::{}", version.repo)), tone_pen(Tone::repo)}};
    if (!version.ebuild) {
        spans.push_back({"installed, no ebuild", tone_pen(Tone::note)});
    } else if (!version.reasons.empty()) {
        std::string reasons;
        for (const auto& reason : version.reasons) {
            reasons += std::format("{}{}", reasons.empty() ? "" : ", ", reason);
        }
        spans.push_back({std::format("masked: {}", reasons), tone_pen(Tone::bad)});
    }
    return spans;
}

namespace {

// What every view that is not taking text has: the prompt and quitting.
void add_common(std::vector<Hint>& keys) {
    keys.push_back({.key = ":", .meaning = "command"});
    keys.push_back({.key = "q", .meaning = "quit"});
}

std::vector<Hint> list_keys(const App& app, const Glyphs& glyph) {
    const auto& list = app.list();
    if (list.searching) {
        return {{.key = std::string{glyph.enter}, .meaning = "keep", .bar = true},
                {.key = "esc", .meaning = "clear", .bar = true},
                {.key = "type", .meaning = "to filter", .bar = true}};
    }
    const bool on_package = list.cursor.at < list.shown.size();
    const bool evaluated = app.has_evaluated();
    std::vector<Hint> keys{{.key = std::string{glyph.move}, .meaning = "move"}};
    if (evaluated || app.notices()) {
        keys.push_back({.key = std::string{glyph.pages}, .meaning = "page"});
    }
    keys.push_back({.key = std::string{glyph.enter}, .meaning = "open", .bar = on_package});
    keys.push_back({.key = "space", .meaning = "pick", .bar = on_package});
    if (evaluated) {
        const bool update = !app.picked().empty() || list.only == Only::updates ||
                            (on_package && app.update_of(list.shown.at(list.cursor.at)));
        keys.push_back({.key = "U", .meaning = "update", .bar = update});
    }
    keys.push_back(
        {.key = "r",
         .meaning = "remove",
         .bar = !app.picked().empty() || (list.only == Only::orphans && !list.shown.empty())});
    const auto filter = [&](std::string_view key, Only only, std::string_view meaning) {
        keys.push_back({.key = std::string{key},
                        .meaning = std::string{list.only == only ? "all" : meaning},
                        .bar = list.only == only});
    };
    filter("o", Only::orphans, "orphans");
    filter("!", Only::broken, "broken");
    if (evaluated) {
        filter("u", Only::updates, "updates");
    }
    keys.push_back({.key = "esc", .meaning = "clear the filter", .bar = !list.query.empty()});
    keys.push_back({.key = "/", .meaning = "filter"});
    keys.push_back({.key = "s", .meaning = "search the repositories"});
    if (evaluated) {
        keys.push_back({.key = "p", .meaning = "plan"});
    }
    keys.push_back(
        {.key = "b", .meaning = app.build_deps() ? "run-time deps only" : "build deps too"});
    keys.push_back({.key = "c", .meaning = "check the store"});
    keys.push_back({.key = "e", .meaning = "running emerges"});
    add_common(keys);
    return keys;
}

std::vector<Hint> page_keys(const App& app, const Glyphs& glyph) {
    const auto& page = app.pages().back();
    const auto row = page.cursor.at < page.rows.size() ? std::optional{page.rows.at(page.cursor.at)}
                                                       : std::nullopt;
    const bool on_link = row && selectable(*row);
    const bool unfolded = on_link && row->unfolded;
    const auto installed = row && row->version ? row->version->installed : std::nullopt;
    const bool opens =
        on_link ? row->link.package != page.package : installed && *installed != page.package;
    const bool on_flag = row && row->flag && !row->flag->fixed;
    std::vector<Hint> keys{
        {.key = std::string{glyph.move}, .meaning = "move"},
        {.key = std::string{glyph.enter}, .meaning = "open", .bar = opens},
        {.key = "space",
         .meaning = on_flag    ? "toggle"
                    : unfolded ? "fold"
                               : "unfold",
         .bar = on_flag || unfolded || (on_link && app.can_unfold(*row))},
        {.key = "*", .meaning = "toggle for every package", .bar = on_flag},
        {.key = "a", .meaning = "try a package.use line"},
        {.key = "e", .meaning = "try a package.env line"},
        {.key = "i", .meaning = "install", .bar = row && row->version.has_value()},
        {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> check_keys(const App& app, const Glyphs& glyph) {
    const auto& checked = *app.checked();
    using Stage = Checked::Stage;
    if (checked.stage == Stage::checking || checked.stage == Stage::rebuilding) {
        return {{.key = "esc", .meaning = "stop", .bar = true},
                {.key = "q", .meaning = "quit", .bar = true}};
    }
    const bool drift = checked.stage != Stage::rebuilt && !checked.drift.empty();
    std::vector<Hint> keys{
        {.key = std::string{glyph.move}, .meaning = "move"},
        {.key = std::string{glyph.enter}, .meaning = "open", .bar = drift},
        {.key = "u", .meaning = app.update() == Update::save ? "rebuild" : "preview", .bar = drift},
        {.key = "r", .meaning = "check again"},
        {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> watch_keys(const App& app, const Glyphs& glyph) {
    const auto& watched = *app.watched();
    if (watched.editing) {
        return {{.key = "h/l", .meaning = "setting", .bar = true},
                {.key = "+/-", .meaning = "change", .bar = true},
                {.key = "s", .meaning = "done", .bar = true},
                {.key = "q", .meaning = "quit", .bar = true}};
    }
    std::vector<Hint> keys{
        {.key = std::string{glyph.move}, .meaning = "move"},
        {.key = std::string{glyph.enter}, .meaning = "open", .bar = !watched.snapshots.empty()},
        {.key = "s", .meaning = "steve", .bar = true},
        {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> search_keys(const App& app, const Glyphs& glyph) {
    const auto& search = *app.search();
    const std::string tab = search.descriptions ? "names only" : "descriptions too";
    if (search.typing) {
        return {{.key = std::string{glyph.enter}, .meaning = "search", .bar = true},
                {.key = "tab", .meaning = tab, .bar = true},
                {.key = "esc", .meaning = "stop", .bar = true}};
    }
    const bool found = !search.results.empty();
    std::vector<Hint> keys{{.key = std::string{glyph.move}, .meaning = "move"},
                           {.key = std::string{glyph.enter}, .meaning = "open", .bar = found},
                           {.key = "i", .meaning = "install", .bar = found},
                           {.key = "/", .meaning = "search again"},
                           {.key = "tab", .meaning = tab},
                           {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> listing_keys(const Glyphs& glyph) {
    std::vector<Hint> keys{{.key = std::string{glyph.move}, .meaning = "move"},
                           {.key = "i", .meaning = "install", .bar = true},
                           {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> output_keys(const App::Output& output, const Glyphs& glyph) {
    const bool linked =
        output.cursor.at < output.links.size() && output.links.at(output.cursor.at).has_value();
    std::vector<Hint> keys{{.key = std::string{glyph.move}, .meaning = "move"},
                           {.key = std::string{glyph.enter}, .meaning = "open", .bar = linked},
                           {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> plan_keys(const Planned& planned, const Glyphs& glyph) {
    std::vector<Hint> keys{
        {.key = std::string{glyph.move}, .meaning = "move"},
        {.key = std::string{glyph.enter}, .meaning = "open", .bar = !planned.rows.empty()},
        {.key = "esc", .meaning = "back", .bar = true}};
    add_common(keys);
    return keys;
}

std::vector<Hint> what_if_keys(const App& app, const Glyphs& glyph) {
    std::vector<Hint> keys{{.key = std::string{glyph.move}, .meaning = "move"},
                           {.key = std::string{glyph.pages}, .meaning = "page"},
                           {.key = "x", .meaning = "drop", .bar = true},
                           {.key = "u", .meaning = "undo", .bar = app.can_undo()},
                           {.key = "a", .meaning = "add a package.use line", .bar = true},
                           {.key = "e", .meaning = "add a package.env line"}};
    if (!app.env_changes().empty()) {
        keys.push_back({.key = "r",
                        .meaning = app.rebuild_env() ? "leave the env rebuilds out"
                                                     : "rebuild what env lines change",
                        .bar = true});
    }
    add_common(keys);
    return keys;
}

std::vector<Hint> notice_keys(const App& app, const Glyphs& glyph) {
    if (app.putting_off()) {
        std::vector<Hint> keys;
        keys.reserve(put_off_choices.size() + 1);
        for (std::size_t choice = 0; choice < put_off_choices.size(); ++choice) {
            keys.push_back({.key = std::to_string(choice + 1),
                            .meaning = std::string{put_off_choices.at(choice).first},
                            .bar = true});
        }
        keys.push_back({.key = "esc", .meaning = "cancel", .bar = true});
        return keys;
    }
    const auto& notices = app.notices();
    const bool on_notice = notices && app.notice_cursor().at < notices->notices.size();
    const auto work =
        on_notice ? notice_work(notices->notices.at(app.notice_cursor().at)) : std::string_view{};
    std::vector<Hint> keys{{.key = std::string{glyph.move}, .meaning = "move"},
                           {.key = std::string{glyph.pages}, .meaning = "page"}};
    if (!work.empty()) {
        keys.push_back(
            {.key = std::string{glyph.enter}, .meaning = std::string{work}, .bar = true});
    }
    keys.insert(keys.end(), {{.key = "x", .meaning = "dismiss", .bar = on_notice},
                             {.key = "z", .meaning = "later", .bar = on_notice}});
    add_common(keys);
    return keys;
}

} // namespace

// In the order draw() picks the view on top.
std::vector<Hint> view_keys(const App& app, const Glyphs& glyph) {
    if (app.typed_line()) {
        return {{.key = std::string{glyph.enter}, .meaning = "try", .bar = true},
                {.key = "esc", .meaning = "cancel", .bar = true}};
    }
    if (!app.pages().empty()) {
        return page_keys(app, glyph);
    }
    if (app.listing()) {
        return listing_keys(glyph);
    }
    if (const auto& output = app.output()) {
        return output_keys(*output, glyph);
    }
    if (app.search()) {
        return search_keys(app, glyph);
    }
    if (app.checked()) {
        return check_keys(app, glyph);
    }
    if (app.watched()) {
        return watch_keys(app, glyph);
    }
    if (const auto& planned = app.planned()) {
        return plan_keys(*planned, glyph);
    }
    if (app.on_what_if()) {
        return what_if_keys(app, glyph);
    }
    if (app.on_notices()) {
        return notice_keys(app, glyph);
    }
    return list_keys(app, glyph);
}

Dialog put_off_dialog() {
    Dialog dialog{.error = false,
                  .title = "Put off",
                  .lines = {},
                  .question = false,
                  .top = 0,
                  .closing = " esc cancel "};
    for (std::size_t choice = 0; choice < put_off_choices.size(); ++choice) {
        dialog.lines.push_back(std::format("{}  {}", choice + 1, put_off_choices.at(choice).first));
    }
    return dialog;
}

Dialog keys_dialog(std::span<const Hint> hints) {
    std::size_t widest = 0;
    for (const auto& hint : hints) {
        widest = std::max(widest, columns(hint.key));
    }
    Dialog dialog{.error = false, .title = "Keys", .lines = {}, .question = false, .top = 0};
    for (const auto& hint : hints) {
        dialog.lines.push_back(std::format(
            "{}{}  {}", hint.key, std::string(widest - columns(hint.key), ' '), hint.meaning));
    }
    return dialog;
}

std::vector<Span> found_spans(const Found& found, const Glyphs& glyph) {
    std::vector<Span> spans{{found.installed.empty() ? std::string{" "} : std::string{glyph.good},
                             tone_pen(Tone::good)},
                            {" ", {}}};
    std::ranges::move(cpv_spans(found.cp), std::back_inserter(spans));
    const auto used = columns(found.cp);
    spans.push_back({std::string(used < 40 ? 42 - used : 2, ' '), {}});
    spans.push_back({std::format("{:<15}", found.version),
                     tone_pen(found.visible ? Tone::version : Tone::bad)});
    spans.push_back({std::format("{:<15}", found.installed), tone_pen(Tone::good)});
    spans.push_back({found.description, tone_pen(Tone::note)});
    return spans;
}

DriftSign drift_sign(char sign) {
    switch (sign) {
    case '+':
        return {.sign = "+", .meaning = "installed since the store was built", .tone = Tone::good};
    case '-':
        return {.sign = "-", .meaning = "gone since the store was built", .tone = Tone::bad};
    default:
        return {.sign = "~", .meaning = "changed since the store was built", .tone = Tone::choice};
    }
}

Exit open_and_run(std::shared_ptr<const Stores> stores, bool dynamic_deps, GlyphSet glyphs,
                  const Services& services, std::span<const std::string> warnings,
                  bool notices_first, std::ostream& err) {
#if EGRAPH_HAVE_TUI
    auto screen = Screen::open();
    if (!screen) {
        err << "egraph: tui: " << screen.error() << '\n';
        return Exit::failure;
    }
    App app{std::move(stores), dynamic_deps, services.rebuild ? Update::save : Update::preview};
    if (!warnings.empty()) {
        app.show({.error = false, .title = "Warning", .lines = {warnings.begin(), warnings.end()}});
    }
    if (notices_first) {
        app.open_notices();
    }
    if (const auto stopped = run(*screen, app, egraph::glyphs(glyphs), services)) {
        err << "egraph: tui: " << *stopped << '\n';
        return Exit::failure;
    }
    return Exit::ok;
#else
    (void)stores;
    (void)dynamic_deps;
    (void)glyphs;
    (void)services;
    (void)warnings;
    (void)notices_first;
    err << "egraph: tui: this egraph was built without Notcurses (meson -Dtui=enabled)\n";
    return Exit::not_implemented;
#endif
}

} // namespace egraph::tui
