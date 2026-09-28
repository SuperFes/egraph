#include "tui.hpp"

#include "build_info.hpp"

#include <iterator>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <tuple>

namespace egraph::tui {

namespace {

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

// Moves over a page, landing only on packages; headings above the first stay in view.
void move_on_page(App::Page& page, const Key& key, std::size_t height) {
    std::vector<std::size_t> stops;
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        if (selectable(page.rows.at(i))) {
            stops.push_back(i);
        }
    }
    if (stops.empty()) {
        return;
    }
    const auto current = std::ranges::lower_bound(stops, page.cursor.at);
    Cursor among{.at = static_cast<std::size_t>(current - stops.begin()), .top = 0};
    among.at = std::min(among.at, stops.size() - 1);
    move(among, stops.size(), key, height);
    page.cursor.at = stops.at(among.at);
    keep_visible(page.cursor, height);
    if (among.at == 0) {
        page.cursor.top = 0;
    }
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

// A build-time dependency whose package is now installed at another version or slot only
// records what the package was built with, not something it lacks.
bool replaced(const Unsatisfied& dependency, std::span<const std::uint32_t> instead) {
    return is_build_kind(dependency.kind) && !instead.empty();
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
    std::vector<bool> later;
    for (auto& row : std::ranges::reverse_view(rows)) {
        const auto depth = selectable(row) ? row.depth : 0;
        later.resize(std::max(later.size(), depth + 1), false);
        row.last = !later.at(depth);
        row.rails.assign(depth > 1 ? depth - 1 : 0, false);
        for (std::size_t level = 1; level < depth; ++level) {
            row.rails.at(level - 1) = later.at(level);
        }
        later.at(depth) = true;
        std::fill(later.begin() + static_cast<std::ptrdiff_t>(depth) + 1, later.end(), false);
    }
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

App::App(const Store& store, const Graph& graph) : store_(store), graph_(graph) {
    const auto count = store.packages.size();
    folded_.reserve(count);
    dependencies_.assign(count, 0);
    dependents_.assign(count, 0);
    broken_.reserve(count);
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
    recompute();
    filter();
}

void App::recompute() {
    kept_ = keep(store(), {.build_deps = build_deps_});
    root_of_.assign(store().packages.size(), std::nullopt);
    for (const auto& pull : kept_.roots) {
        auto& root = root_of_.at(pull.child);
        if (!root || pull.root < *root) {
            root = pull.root;
        }
    }
}

void App::filter() {
    const auto query = folded(list_.query);
    list_.shown.clear();
    for (std::uint32_t id = 0; id < folded_.size(); ++id) {
        if ((list_.only == Only::orphans && kept_.packages.at(id)) ||
            (list_.only == Only::broken && broken(id) == 0)) {
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
    std::ranges::move(unsatisfied_rows(store(), package, build_deps_),
                      std::back_inserter(page.rows));
    std::ranges::move(page_rows(store(), graph_.get(), package), std::back_inserter(page.rows));
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

void App::handle(const Key& key) {
    if (key.kind == KeyKind::closed) {
        done_ = true;
    } else if (!pages_.empty()) {
        handle_page(key);
    } else if (checked_) {
        handle_check(key);
    } else {
        handle_list(key);
    }
}

void App::finish_check(CheckResult result) {
    if (!checked_) {
        return;
    }
    checked_->running = false;
    checked_->cursor = {};
    if (result) {
        checked_->error.clear();
        checked_->drift = std::move(*result);
    } else {
        checked_->error = std::move(result.error());
        checked_->drift.clear();
    }
}

std::optional<std::uint32_t> App::find(std::string_view cpv) const {
    for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
        if (store().string(store().packages.at(id).cpv) == cpv) {
            return id;
        }
    }
    return std::nullopt;
}

void App::handle_check(const Key& key) {
    if (!checked_ || checked_->running) {
        return;
    }
    auto& checked = *checked_;
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        checked_.reset();
    } else if (is(key, U'r')) {
        checked = {};
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        // Packages only a fresh build has are not in the store to open.
        if (checked.cursor.at < checked.drift.size()) {
            if (const auto id =
                    find(std::string_view{checked.drift.at(checked.cursor.at)}.substr(1))) {
                open(*id);
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
    } else if (is(key, U'o')) {
        list_.only = list_.only == Only::orphans ? Only::all : Only::orphans;
        filter();
    } else if (is(key, U'!')) {
        list_.only = list_.only == Only::broken ? Only::all : Only::broken;
        filter();
    } else if (is(key, U'c')) {
        checked_.emplace();
    } else if (is(key, U'b')) {
        build_deps_ = !build_deps_;
        recompute();
        filter();
    } else if (key.kind == KeyKind::escape && !list_.query.empty()) {
        list_.query.clear();
        filter();
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (list_.cursor.at < list_.shown.size()) {
            open(list_.shown.at(list_.cursor.at));
        }
    } else if (is_move(key)) {
        move(list_.cursor, list_.shown.size(), key, height_);
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
        if (key.kind == KeyKind::left || is(key, U'h')) {
            pages_.pop_back();
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

Exit open_and_run(const Store& store, GlyphSet glyphs, const Checker& check, std::ostream& err) {
#if EGRAPH_HAVE_TUI
    auto screen = Screen::open();
    if (!screen) {
        err << "egraph: tui: " << screen.error() << '\n';
        return Exit::failure;
    }
    const auto graph = build_graph(store);
    App app{store, graph};
    run(*screen, app, egraph::glyphs(glyphs), check);
    return Exit::ok;
#else
    (void)store;
    (void)glyphs;
    (void)check;
    err << "egraph: tui: this egraph was built without Notcurses (meson -Dtui=enabled)\n";
    return Exit::not_implemented;
#endif
}

} // namespace egraph::tui
