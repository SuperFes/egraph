#include "tui.hpp"

#include "build_info.hpp"

#include <ostream>
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

// Moves over a page, landing only on links; headings above the first link stay in view.
void move_on_page(App::Page& page, const Key& key, std::size_t height) {
    std::vector<std::size_t> selectable;
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        if (page.rows.at(i).type == RowType::link) {
            selectable.push_back(i);
        }
    }
    if (selectable.empty()) {
        return;
    }
    const auto current = std::ranges::lower_bound(selectable, page.cursor.at);
    Cursor among{.at = static_cast<std::size_t>(current - selectable.begin()), .top = 0};
    among.at = std::min(among.at, selectable.size() - 1);
    move(among, selectable.size(), key, height);
    page.cursor.at = selectable.at(among.at);
    keep_visible(page.cursor, height);
    if (among.at == 0) {
        page.cursor.top = 0;
    }
}

std::vector<Row> page_rows(const Store& store, const Graph& graph, std::uint32_t package) {
    std::vector<Row> rows;
    for (const bool reverse : {false, true}) {
        const auto found = links(store, graph, package, reverse);
        if (reverse) {
            rows.push_back({.type = RowType::note, .text = "", .link = {}});
        }
        rows.push_back(
            {.type = RowType::heading,
             .text = std::format("{}  {}", reverse ? "Needed by" : "Depends on", found.size()),
             .link = {}});
        if (found.empty()) {
            rows.push_back({.type = RowType::note, .text = "nothing", .link = {}});
        }
        for (const auto& link : found) {
            rows.push_back({.type = RowType::link, .text = "", .link = link});
        }
    }
    return rows;
}

} // namespace

bool available() {
    return EGRAPH_HAVE_TUI != 0;
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
        for (const auto child : children) {
            ++dependents_.at(child);
        }
    }
    filter();
}

void App::filter() {
    const auto query = folded(list_.query);
    list_.shown.clear();
    for (std::uint32_t id = 0; id < folded_.size(); ++id) {
        if (folded_.at(id).find(query) != std::string::npos) {
            list_.shown.push_back(id);
        }
    }
    list_.cursor = {};
}

void App::open(std::uint32_t package) {
    Page page{.package = package, .rows = page_rows(store(), graph_.get(), package), .cursor = {}};
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        if (page.rows.at(i).type == RowType::link) {
            page.cursor.at = i;
            break;
        }
    }
    keep_visible(page.cursor, height_);
    if (page.cursor.at < height_) {
        page.cursor.top = 0;
    }
    pages_.push_back(std::move(page));
}

void App::handle(const Key& key) {
    if (key.kind == KeyKind::closed) {
        done_ = true;
    } else if (pages_.empty()) {
        handle_list(key);
    } else {
        handle_page(key);
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

void App::handle_page(const Key& key) {
    auto& page = pages_.back();
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        pages_.pop_back();
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (page.cursor.at < page.rows.size() &&
            page.rows.at(page.cursor.at).type == RowType::link) {
            open(page.rows.at(page.cursor.at).link.package);
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

Exit open_and_run(const Store& store, GlyphSet glyphs, std::ostream& err) {
#if EGRAPH_HAVE_TUI
    auto screen = Screen::open();
    if (!screen) {
        err << "egraph: tui: " << screen.error() << '\n';
        return Exit::failure;
    }
    const auto graph = build_graph(store);
    App app{store, graph};
    run(*screen, app, egraph::glyphs(glyphs));
    return Exit::ok;
#else
    (void)store;
    (void)glyphs;
    err << "egraph: tui: this egraph was built without Notcurses (meson -Dtui=enabled)\n";
    return Exit::not_implemented;
#endif
}

} // namespace egraph::tui
