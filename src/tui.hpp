#pragma once

// The terminal interface. App is the state and what keys do to it, with no terminal in sight;
// draw() and run() are templates over the screen, so tests drive them with a fake one and only
// tui.cpp pairs them with the Notcurses Screen.

#include "cli.hpp"
#include "depclean.hpp"
#include "graph.hpp"
#include "human.hpp"
#include "query.hpp"
#include "screen.hpp"
#include "store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace egraph::tui {

// Built with Notcurses; without it, open_and_run only reports that.
[[nodiscard]] bool available();

// Catppuccin Mocha, as the human layout uses.
namespace palette {
inline constexpr Color crust{.red = 17, .green = 17, .blue = 27};
inline constexpr Color mantle{.red = 24, .green = 24, .blue = 37};
inline constexpr Color surface{.red = 49, .green = 50, .blue = 68};
inline constexpr Color text{.red = 205, .green = 214, .blue = 244};
inline constexpr Color overlay{.red = 127, .green = 132, .blue = 156};
inline constexpr Color mauve{.red = 203, .green = 166, .blue = 247};
} // namespace palette

// One package another depends on (or is depended on by) through one atom, with the kinds it
// appears under, in kind_shorthands order.
struct Link {
    std::uint32_t package = 0;
    std::uint32_t atom = 0;
    bool choice = false;
    std::array<bool, kind_shorthands.size()> kinds{};
};

// deps (reverse false) or rdeps of a package, one Link per package and atom, in cpv order.
[[nodiscard]] std::vector<Link> links(const Store& store, const Graph& graph, std::uint32_t package,
                                      bool reverse);

// A root row names a root set and atom; path rows are the chain that root keeps the page
// package through, the link holding the dependency that pulls each one in.
// An alert is a note that needs attention; a missing row is an unsatisfied dependency, its text
// rendered as portage does and its kinds in link.kinds.
enum class RowType : std::uint8_t { heading, note, alert, missing, link, root, path };

// A page row. Links at depth 0 are the page package's own; unfolding a link puts its links,
// in the same direction, one level deeper right below it.
struct Row {
    RowType type = RowType::note;
    std::string text;
    Link link;
    std::size_t depth = 0;
    bool reverse = false;
    bool unfolded = false;
    // The link's package is already on the path from the page package down to it.
    bool cycle = false;
    // Tree lines: whether this is its parent's last child, and for each level from 1 to depth - 1
    // whether that level's line continues past this row.
    bool last = false;
    std::vector<bool> rails;
};

// Sets each row's last and rails, walking up from the bottom: a level's line continues past a
// row when a sibling at that level follows before anything shallower does.
void thread(std::vector<Row>& rows);

// A scrolling list's selected entry and first visible one.
struct Cursor {
    std::size_t at = 0;
    std::size_t top = 0;
};

// Which packages the list shows, before the search.
enum class Only : std::uint8_t { all, orphans, broken };

class App {
  public:
    App(const Store& store, const Graph& graph);

    void handle(const Key& key);
    [[nodiscard]] bool done() const { return done_; }

    // The package list: every installed package whose cpv contains the query.
    struct List {
        std::string query;
        bool searching = false;
        Only only = Only::all;
        std::vector<std::uint32_t> shown;
        Cursor cursor;
    };
    // One package's page: why depclean keeps it, what it needs that is not installed, what it
    // depends on, then what depends on it, the last two as trees that unfold.
    struct Page {
        std::uint32_t package = 0;
        std::vector<Row> rows;
        Cursor cursor;
    };

    [[nodiscard]] const Store& store() const { return store_.get(); }
    [[nodiscard]] const List& list() const { return list_; }
    // Pages opened from the list, the one showing last; empty on the list.
    [[nodiscard]] const std::vector<Page>& pages() const { return pages_; }
    // Distinct packages each package depends on, and that depend on it.
    [[nodiscard]] std::size_t dependencies(std::uint32_t package) const {
        return dependencies_.at(package);
    }
    [[nodiscard]] std::size_t dependents(std::uint32_t package) const {
        return dependents_.at(package);
    }
    // Top-level dependencies of a package that nothing installed satisfies; only run-time ones
    // without build_deps.
    [[nodiscard]] std::size_t broken(std::uint32_t package) const {
        return (build_deps_ ? broken_ : broken_at_run_time_).at(package);
    }
    // What depclean keeps, and whether it follows build-time dependencies.
    [[nodiscard]] const Kept& kept() const { return kept_; }
    [[nodiscard]] bool build_deps() const { return build_deps_; }
    // The first root that keeps a package directly, as an index into Store::roots.
    [[nodiscard]] std::optional<std::uint32_t> root_of(std::uint32_t package) const {
        return root_of_.at(package);
    }
    // Whether a link can unfold: it is no cycle and its package has links that way.
    [[nodiscard]] bool can_unfold(const Row& row) const;
    // Rows the list or page has on screen; drawing sets it, and paging and scrolling use it.
    void set_height(std::size_t rows) { height_ = std::max<std::size_t>(rows, 1); }

  private:
    void filter();
    void recompute();
    void open(std::uint32_t package);
    void unfold(Page& page);
    void fold(Page& page);
    void handle_list(const Key& key);
    void handle_page(const Key& key);

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Graph> graph_;
    std::vector<std::string> folded_;
    std::vector<std::size_t> dependencies_;
    std::vector<std::size_t> dependents_;
    std::vector<std::size_t> broken_;
    std::vector<std::size_t> broken_at_run_time_;
    bool build_deps_ = true;
    Kept kept_;
    std::vector<std::optional<std::uint32_t>> root_of_;
    List list_;
    std::vector<Page> pages_;
    std::size_t height_ = 1;
    bool done_ = false;
};

// A piece of text in one pen, for putting several side by side.
struct Span {
    std::string text;
    Pen pen;
};

// Columns text takes: one per code point, which holds for everything egraph shows.
[[nodiscard]] std::size_t columns(std::string_view text);

// text cut to at most width columns, at a code point boundary.
[[nodiscard]] std::string clip(std::string_view text, std::size_t width);

[[nodiscard]] Pen tone_pen(Tone tone);
[[nodiscard]] std::vector<Span> cpv_spans(std::string_view cpv);

// Puts spans from col on, clipped at width columns, over background bg when set.
template <class S>
void put_spans(S& screen, unsigned row, unsigned col, const std::vector<Span>& spans,
               unsigned width, std::optional<Color> bg = std::nullopt) {
    for (const auto& span : spans) {
        if (col >= width) {
            return;
        }
        auto pen = span.pen;
        if (bg) {
            pen.bg = bg;
        }
        const auto text = clip(span.text, width - col);
        screen.put(row, col, text, pen);
        col += static_cast<unsigned>(columns(text));
    }
}

// The hint bar: pairs of key and what it does.
template <class S>
void draw_hints(S& screen, unsigned row, unsigned width,
                const std::vector<std::pair<std::string_view, std::string_view>>& hints) {
    const Pen bar{.fg = palette::overlay, .bg = palette::mantle};
    screen.fill_row(row, bar);
    std::vector<Span> spans{{" ", bar}};
    for (const auto& [key, meaning] : hints) {
        spans.push_back(
            {std::string{key}, {.fg = palette::mauve, .bg = palette::mantle, .bold = true}});
        spans.push_back({std::format(" {}   ", meaning), bar});
    }
    put_spans(screen, row, 0, spans, width);
}

template <class S> void draw_title(S& screen, unsigned width, const std::vector<Span>& trail) {
    screen.fill_row(0, {.fg = palette::text, .bg = palette::crust});
    put_spans(screen, 0, 0, trail, width, palette::crust);
}

// The marker in front of the selected row.
inline Span marker(bool selected, const Glyphs& glyph) {
    return {selected ? std::format(" {} ", glyph.cursor) : "   ",
            {.fg = palette::mauve, .bg = std::nullopt, .bold = true}};
}

// A package's marks in the list: the root set that keeps it, or that depclean would remove it;
// then whether it has unsatisfied dependencies.
inline Span keep_mark(const App& app, std::uint32_t package, const Glyphs& glyph) {
    if (const auto root = app.root_of(package)) {
        const auto set = std::format("@{}", app.store().string(app.store().roots.at(*root).set));
        return {std::string{set_glyph(set, glyph)}, tone_pen(Tone::root)};
    }
    if (!app.kept().packages.at(package)) {
        return {std::string{glyph.orphan}, tone_pen(Tone::bad)};
    }
    return {" ", {}};
}

inline Span broken_mark(const App& app, std::uint32_t package, const Glyphs& glyph) {
    if (app.broken(package) > 0) {
        return {std::string{glyph.broken}, tone_pen(Tone::bad)};
    }
    return {" ", {}};
}

inline std::string_view empty_list(const App::List& list) {
    if (!list.query.empty() || list.only == Only::all) {
        return "no package matches";
    }
    return list.only == Only::orphans ? "nothing to remove" : "nothing broken";
}

template <class S> void draw_list(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& list = app.list();
    const auto& store = app.store();
    std::vector<Span> title{{std::format(" {} egraph ", glyph.package),
                             {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                            {std::format(" {}  ", store.meta.eroot), tone_pen(Tone::note)}};
    if (list.only == Only::orphans) {
        title.push_back({std::format("{} orphans", list.shown.size()), tone_pen(Tone::count)});
    } else if (list.only == Only::broken) {
        title.push_back({std::format("{} broken", list.shown.size()), tone_pen(Tone::count)});
    } else {
        title.push_back({std::format("{} of {} packages", list.shown.size(), store.packages.size()),
                         tone_pen(Tone::count)});
    }
    if (!app.build_deps()) {
        title.push_back({"  run-time deps only", tone_pen(Tone::note)});
    }
    if (list.only == Only::orphans && (store.roots.empty() || !app.kept().unresolved.empty())) {
        title.push_back(
            {std::format("  {} depclean would refuse to run", glyph.broken), tone_pen(Tone::bad)});
    }
    draw_title(screen, size.cols, title);

    std::vector<Span> search{{std::format(" {} ", glyph.search), tone_pen(Tone::heading)}};
    if (list.searching || !list.query.empty()) {
        search.push_back({list.query, {.fg = palette::text, .bg = std::nullopt, .bold = true}});
        if (list.searching) {
            search.push_back({" ", {.fg = std::nullopt, .bg = palette::mauve}});
        }
    } else {
        search.push_back({"/ to search", tone_pen(Tone::note)});
    }
    put_spans(screen, 1, 0, search, size.cols);

    const unsigned first = 3;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    const unsigned right = size.cols > 20 ? size.cols - 20 : 0;
    put_spans(screen, 2, 0, {{"      package", tone_pen(Tone::note)}}, size.cols);
    put_spans(screen, 2, right, {{"   deps  needed by", tone_pen(Tone::note)}}, size.cols);
    for (unsigned line = 0; line < height; ++line) {
        const auto index = list.cursor.top + line;
        if (index >= list.shown.size()) {
            break;
        }
        const auto id = list.shown.at(index);
        const bool selected = index == list.cursor.at;
        const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
        if (selected) {
            screen.fill_row(first + line, {.fg = std::nullopt, .bg = palette::surface});
        }
        std::vector<Span> spans{marker(selected, glyph),
                                keep_mark(app, id, glyph),
                                broken_mark(app, id, glyph),
                                {" ", {}}};
        std::ranges::move(cpv_spans(store.string(store.packages.at(id).cpv)),
                          std::back_inserter(spans));
        put_spans(screen, first + line, 0, spans, right, bg);
        put_spans(screen, first + line, right,
                  {{std::format("{:>7}", app.dependencies(id)), tone_pen(Tone::version)},
                   {std::format("{:>11}", app.dependents(id)), tone_pen(Tone::count)}},
                  size.cols, bg);
    }
    if (list.shown.empty()) {
        put_spans(screen, first, 5, {{std::string{empty_list(list)}, tone_pen(Tone::note)}},
                  size.cols);
    }
    if (list.searching) {
        draw_hints(screen, size.rows - 1, size.cols,
                   {{glyph.enter, "keep"}, {"esc", "clear"}, {"type", "to filter"}});
    } else {
        draw_hints(screen, size.rows - 1, size.cols,
                   {{glyph.move, "move"},
                    {glyph.enter, "open"},
                    {"/", "search"},
                    {"o", list.only == Only::orphans ? "all" : "orphans"},
                    {"!", list.only == Only::broken ? "all" : "broken"},
                    {"b", app.build_deps() ? "run time only" : "build deps"},
                    {"q", "quit"}});
    }
}

// The column before a link's package: whether it unfolds, is unfolded, or closes a cycle.
inline Span fold_span(const App& app, const Row& row, const Glyphs& glyph) {
    if (row.cycle) {
        return {std::string{glyph.cycle}, tone_pen(Tone::bad)};
    }
    if (row.unfolded) {
        return {std::string{glyph.unfolded},
                {.fg = palette::mauve, .bg = std::nullopt, .bold = true}};
    }
    if (app.can_unfold(row)) {
        return {std::string{glyph.folded}, tone_pen(Tone::note)};
    }
    return {" ", {}};
}

template <class S> void draw_page(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& store = app.store();
    const auto& page = app.pages().back();
    // The trail of opened packages, by name.
    std::vector<Span> trail{{std::format(" {} egraph ", glyph.package),
                             {.fg = palette::mauve, .bg = std::nullopt, .bold = true}}};
    for (const auto& opened : app.pages()) {
        const auto parts = split_cpv(store.string(store.packages.at(opened.package).cpv));
        trail.push_back({std::format(" {} ", glyph.trail), tone_pen(Tone::note)});
        trail.push_back({std::string{parts.name},
                         &opened == &page ? tone_pen(Tone::name) : tone_pen(Tone::category)});
    }
    draw_title(screen, size.cols, trail);
    auto heading = cpv_spans(store.string(store.packages.at(page.package).cpv));
    heading.insert(heading.begin(), {std::format(" {} ", glyph.package), tone_pen(Tone::heading)});
    put_spans(screen, 1, 0, heading, size.cols);

    const unsigned first = 3;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    for (unsigned line = 0; line < height; ++line) {
        const auto index = page.cursor.top + line;
        if (index >= page.rows.size()) {
            break;
        }
        const auto& row = page.rows.at(index);
        const unsigned at = first + line;
        switch (row.type) {
        case RowType::heading:
            put_spans(screen, at, 1, {{row.text, tone_pen(Tone::heading)}}, size.cols);
            break;
        case RowType::note:
            put_spans(screen, at, 3, {{row.text, tone_pen(Tone::note)}}, size.cols);
            break;
        case RowType::alert:
            put_spans(screen, at, 3,
                      {{std::format("{} {}", glyph.orphan, row.text), tone_pen(Tone::bad)}},
                      size.cols);
            break;
        case RowType::missing: {
            std::vector<Span> spans{{"   ", {}}};
            for (std::size_t k = 0; k < kind_shorthands.size(); ++k) {
                const auto& kind = kind_shorthands.at(k);
                spans.push_back(row.link.kinds.at(k)
                                    ? Span{std::string{kind.letter}, tone_pen(kind.tone)}
                                    : Span{std::string{glyph.absent}, tone_pen(Tone::note)});
            }
            spans.push_back({std::format(" {} ", glyph.broken), tone_pen(Tone::bad)});
            spans.push_back({row.text, tone_pen(Tone::bad)});
            put_spans(screen, at, 0, spans, size.cols);
            break;
        }
        case RowType::root:
            put_spans(
                screen, at, 3,
                {{std::format("{} {}", set_glyph(row.text, glyph), row.text), tone_pen(Tone::root)},
                 {"  ", {}},
                 {std::string{store.string(row.link.atom)}, tone_pen(Tone::note)}},
                size.cols);
            break;
        case RowType::path:
        case RowType::link: {
            const bool selected = index == page.cursor.at;
            const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
            if (selected) {
                screen.fill_row(at, {.fg = std::nullopt, .bg = palette::surface});
            }
            std::vector<Span> spans{marker(selected, glyph)};
            for (std::size_t k = 0; k < kind_shorthands.size(); ++k) {
                const auto& kind = kind_shorthands.at(k);
                spans.push_back(row.link.kinds.at(k)
                                    ? Span{std::string{kind.letter}, tone_pen(kind.tone)}
                                    : Span{std::string{glyph.absent}, tone_pen(Tone::note)});
            }
            spans.push_back({" ", {}});
            spans.push_back(fold_span(app, row, glyph));
            spans.push_back({" ", {}});
            std::string tree;
            if (row.depth > 0) {
                for (const bool rail : row.rails) {
                    tree += rail ? glyph.rail : "  ";
                }
                tree += std::format("{} ", row.last ? glyph.branch : glyph.tee);
            }
            spans.push_back({tree, tone_pen(Tone::note)});
            const auto cpv = store.string(store.packages.at(row.link.package).cpv);
            for (auto& span : cpv_spans(cpv)) {
                spans.push_back(std::move(span));
            }
            const auto used = columns(tree) + columns(cpv);
            spans.push_back({std::string(used < 40 ? 42 - used : 2, ' '), {}});
            spans.push_back({std::string{store.string(row.link.atom)}, tone_pen(Tone::note)});
            if (row.link.choice) {
                spans.push_back({std::format(" {}", glyph.choice), tone_pen(Tone::choice)});
            }
            put_spans(screen, at, 0, spans, size.cols, bg);
            break;
        }
        }
    }
    draw_hints(screen, size.rows - 1, size.cols,
               {{glyph.move, "move"},
                {"space", "unfold"},
                {glyph.enter, "open"},
                {"esc", "back"},
                {"q", "quit"}});
}

template <class S> void draw(S& screen, App& app, const Glyphs& glyph) {
    const auto size = screen.size();
    screen.clear();
    if (size.rows >= 5 && size.cols >= 10) {
        if (app.pages().empty()) {
            draw_list(screen, app, glyph, size);
        } else {
            draw_page(screen, app, glyph, size);
        }
    }
    screen.render();
}

// Runs until the user quits or input ends.
template <class S> void run(S& screen, App& app, const Glyphs& glyph) {
    draw(screen, app, glyph);
    while (!app.done()) {
        app.handle(screen.read());
        if (!app.done()) {
            draw(screen, app, glyph);
        }
    }
}

// Opens the terminal and runs the interface over store; errors go to err.
[[nodiscard]] Exit open_and_run(const Store& store, GlyphSet glyphs, std::ostream& err);

} // namespace egraph::tui
