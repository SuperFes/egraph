#pragma once

// The terminal interface. App is the state and what keys do to it, with no terminal in sight;
// draw() and run() are templates over the screen, so tests drive them with a fake one and only
// tui.cpp pairs them with the Notcurses Screen.

#include "cli.hpp"
#include "depclean.hpp"
#include "emerge.hpp"
#include "graph.hpp"
#include "human.hpp"
#include "pressure.hpp"
#include "query.hpp"
#include "screen.hpp"
#include "store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
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
// An alert is a note that needs attention. A missing row is an unsatisfied dependency, its text
// rendered as portage does and its kinds in link.kinds; a replaced row is a build-time one whose
// package is now installed at another version or slot.
enum class RowType : std::uint8_t { heading, note, alert, missing, replaced, link, root, path };

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
    // For missing and replaced rows: installed packages with the dependency's name.
    std::vector<std::uint32_t> instead;
};

// Sets each row's last and rails, walking up from the bottom: a level's line continues past a
// row when a sibling at that level follows before anything shallower does.
void thread(std::vector<Row>& rows);

// A scrolling list's selected entry and first visible one.
struct Cursor {
    std::size_t at = 0;
    std::size_t top = 0;
};

// A fresh build, and how the store differs from it: drift lines ("+cpv", "-cpv", "~cpv").
struct Fresh {
    Store store;
    std::vector<std::string> drift;
};
// What `egraph check` finds, or why it could not run.
using CheckResult = std::expected<Fresh, std::string>;
// Builds a fresh store and compares the given one with it.
using Checker = std::function<CheckResult(const Store&)>;
// Writes a fresh store over the one on disk and loads it, or says why it could not.
using Rebuilder = std::function<std::expected<Store, std::string>()>;

// Reads the running emerges' snapshots.
using Watcher = std::function<std::vector<emerge::Snapshot>()>;

// Reads /proc for the pressure graphs.
using Sampler = std::function<pressure::Sample()>;

// What the interface asks of the world outside it: run() calls these, the app never does.
struct Services {
    Checker check{};
    // Empty where the store cannot be written, so that a check's fresh build is only previewed.
    Rebuilder rebuild{};
    Watcher watch{};
    Sampler sample{};
};

// Where steve stops handing out jobs, marked on the pressure graphs when known.
struct Limits {
    std::optional<double> load{};
    std::optional<std::uint64_t> min_available{};
};

// How often the emerge view reads the snapshots again.
inline constexpr std::chrono::milliseconds watch_interval{1000};

// The emerge view: the running emerges as last read, and whether a read is due.
struct Watched {
    std::vector<emerge::Snapshot> snapshots;
    bool due = true;
    // Reads so far, which turn the spinners.
    std::size_t frame = 0;
    pressure::History history;
    Limits limits;
    // Over the tasks of every emerge, in order.
    Cursor cursor;
};

// What u does in the check view: rebuild the store on disk and show it, or only show the check's
// fresh build, for users who cannot write the store.
enum class Update : std::uint8_t { save, preview };
// The store on screen: the one opened, one rebuilt since, or a fresh build that is not saved.
enum class Source : std::uint8_t { opened, saved, preview };

// The check view: waiting for a fresh build or a rebuild, or what either found. Either failing
// shows a dialog instead.
struct Checked {
    enum class Stage : std::uint8_t { checking, checked, rebuilding, rebuilt };
    Stage stage = Stage::checking;
    std::vector<std::string> drift;
    // The check's build, kept for a preview.
    std::optional<Store> fresh;
    Cursor cursor;
};

// A message over whatever is on screen, which the next key dismisses.
struct Dialog {
    bool error = true;
    std::string title;
    std::vector<std::string> lines;
};

// Which packages the list shows, before the search.
enum class Only : std::uint8_t { all, orphans, broken };

class App {
  public:
    App(const Store& store, const Graph& graph, Update update = Update::preview);

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
    // Pages opened from the list or the check view, the one showing last.
    [[nodiscard]] const std::vector<Page>& pages() const { return pages_; }
    // Open from the list, under any pages opened from it.
    [[nodiscard]] const std::optional<Checked>& checked() const { return checked_; }
    // Whether the check view waits for a fresh build, which run() then makes.
    [[nodiscard]] bool check_requested() const {
        return checked_ && checked_->stage == Checked::Stage::checking;
    }
    void finish_check(CheckResult result);
    // Whether the check view waits for a rebuild, which run() then makes.
    [[nodiscard]] bool rebuild_requested() const {
        return checked_ && checked_->stage == Checked::Stage::rebuilding;
    }
    void finish_rebuild(std::expected<Store, std::string> result);
    // Open from the list, under any pages opened from it.
    [[nodiscard]] const std::optional<Watched>& watched() const { return watched_; }
    // Whether the emerge view is showing and due to read the snapshots, which run() then does.
    [[nodiscard]] bool watch_requested() const;
    void finish_watch(std::vector<emerge::Snapshot> snapshots, const pressure::Sample& sample = {});
    // How long run() waits for a key before the view needs drawing again; unset waits for one.
    [[nodiscard]] std::optional<std::chrono::milliseconds> refresh() const;
    [[nodiscard]] const std::optional<Dialog>& dialog() const { return dialog_; }
    void show(Dialog dialog) { dialog_ = std::move(dialog); }
    [[nodiscard]] Update update() const { return update_; }
    [[nodiscard]] Source source() const { return source_; }
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
    // A store the app replaced the opened one with, and its graph.
    struct Loaded {
        explicit Loaded(Store fresh) : store(std::move(fresh)), graph(build_graph(store)) {}
        Store store;
        Graph graph;
    };

    void index();
    // Shows store in place of the current one, back at the list (or the check view).
    void adopt(Store store, Source source);
    void filter();
    void recompute();
    void open(std::uint32_t package);
    void unfold(Page& page);
    void fold(Page& page);
    void handle_list(const Key& key);
    void handle_page(const Key& key);
    void handle_check(const Key& key);
    void handle_watch(const Key& key);
    // The package with this cpv, if the store has it.
    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view cpv) const;

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Graph> graph_;
    // Behind a pointer so store_ and graph_ stay valid when the app moves.
    std::unique_ptr<const Loaded> owned_;
    Update update_;
    Source source_ = Source::opened;
    std::vector<std::string> folded_;
    std::vector<std::size_t> dependencies_;
    std::vector<std::size_t> dependents_;
    std::vector<std::size_t> broken_;
    std::vector<std::size_t> broken_at_run_time_;
    bool build_deps_ = true;
    Kept kept_;
    std::vector<std::optional<std::uint32_t>> root_of_;
    List list_;
    std::optional<Checked> checked_;
    std::optional<Watched> watched_;
    std::vector<Page> pages_;
    std::optional<Dialog> dialog_;
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
        spans.push_back({std::format(" {}  ", meaning), bar});
    }
    put_spans(screen, row, 0, spans, width);
}

// The trail, and on the right a warning while a fresh build is shown without being saved.
template <class S>
void draw_title(S& screen, const App& app, unsigned width, const std::vector<Span>& trail) {
    screen.fill_row(0, {.fg = palette::text, .bg = palette::crust});
    put_spans(screen, 0, 0, trail, width, palette::crust);
    if (app.source() == Source::preview) {
        const std::string badge = " preview, not saved ";
        if (const auto used = columns(badge); used < width) {
            put_spans(screen, 0, width - static_cast<unsigned>(used),
                      {{badge, {.fg = palette::crust, .bg = palette::mauve, .bold = true}}}, width);
        }
    }
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
    draw_title(screen, app, size.cols, title);

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
                    {"c", "check"},
                    {"e", "emerges"},
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
    draw_title(screen, app, size.cols, trail);
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
        case RowType::missing:
        case RowType::replaced: {
            const bool bad = row.type == RowType::missing;
            std::vector<Span> spans{{"   ", {}}};
            for (std::size_t k = 0; k < kind_shorthands.size(); ++k) {
                const auto& kind = kind_shorthands.at(k);
                spans.push_back(row.link.kinds.at(k)
                                    ? Span{std::string{kind.letter}, tone_pen(kind.tone)}
                                    : Span{std::string{glyph.absent}, tone_pen(Tone::note)});
            }
            spans.push_back({std::format(" {} ", bad ? glyph.broken : " "), tone_pen(Tone::bad)});
            spans.push_back({row.text, tone_pen(bad ? Tone::bad : Tone::note)});
            if (!row.instead.empty()) {
                spans.push_back({std::format("  {} ", glyph.instead), tone_pen(Tone::note)});
                for (std::size_t i = 0; i < row.instead.size(); ++i) {
                    if (i > 0) {
                        spans.push_back({", ", tone_pen(Tone::note)});
                    }
                    std::ranges::move(
                        cpv_spans(store.string(store.packages.at(row.instead.at(i)).cpv)),
                        std::back_inserter(spans));
                }
            }
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

// A drift line's sign, as a glyph and what it means.
struct DriftSign {
    std::string_view sign;
    std::string_view meaning;
    Tone tone;
};

[[nodiscard]] DriftSign drift_sign(char sign);

template <class S> void draw_check(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& open = app.checked();
    if (!open) {
        return;
    }
    const auto& checked = *open;
    draw_title(screen, app, size.cols,
               {{std::format(" {} egraph ", glyph.package),
                 {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                {std::format(" {} ", glyph.trail), tone_pen(Tone::note)},
                {"check", tone_pen(Tone::name)}});
    const unsigned first = 3;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    using Stage = Checked::Stage;
    if (checked.stage == Stage::checking || checked.stage == Stage::rebuilding) {
        put_spans(screen, 1, 1,
                  {{checked.stage == Stage::checking
                        ? "Building a fresh store to compare with; this takes a few seconds"
                        : "Rebuilding the store; this takes a few seconds",
                    tone_pen(Tone::note)}},
                  size.cols);
        draw_hints(screen, size.rows - 1, size.cols, {});
        return;
    }
    const bool rebuilt = checked.stage == Stage::rebuilt;
    if (rebuilt && app.source() == Source::preview) {
        put_spans(screen, 1, 1,
                  {{std::format("{} Showing the fresh build", glyph.good), tone_pen(Tone::good)},
                   {"   not saved: only root writes the store", tone_pen(Tone::note)}},
                  size.cols);
    } else if (rebuilt) {
        put_spans(screen, 1, 1,
                  {{std::format("{} Rebuilt the store", glyph.good), tone_pen(Tone::good)},
                   {"   showing it now", tone_pen(Tone::note)}},
                  size.cols);
    } else if (checked.drift.empty()) {
        put_spans(
            screen, 1, 1,
            {{std::format("{} The store matches a fresh build", glyph.good), tone_pen(Tone::good)}},
            size.cols);
    } else {
        put_spans(screen, 1, 1,
                  {{std::format("The store differs from a fresh build  {}", checked.drift.size()),
                    tone_pen(Tone::heading)},
                   {app.update() == Update::save ? "   u rebuilds it"
                                                 : "   u shows the fresh build, without saving it",
                    tone_pen(Tone::note)}},
                  size.cols);
        for (unsigned line = 0; line < height; ++line) {
            const auto index = checked.cursor.top + line;
            if (index >= checked.drift.size()) {
                break;
            }
            const auto& text = checked.drift.at(index);
            const auto sign = drift_sign(text.front());
            const bool selected = index == checked.cursor.at;
            const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
            if (selected) {
                screen.fill_row(first + line, {.fg = std::nullopt, .bg = palette::surface});
            }
            std::vector<Span> spans{marker(selected, glyph),
                                    {std::format("{} ", sign.sign), tone_pen(sign.tone)}};
            const auto cpv = std::string_view{text}.substr(1);
            std::ranges::move(cpv_spans(cpv), std::back_inserter(spans));
            spans.push_back({std::string(columns(cpv) < 44 ? 46 - columns(cpv) : 2, ' '), {}});
            spans.push_back({std::string{sign.meaning}, tone_pen(Tone::note)});
            put_spans(screen, first + line, 0, spans, size.cols, bg);
        }
    }
    std::vector<std::pair<std::string_view, std::string_view>> hints{
        {"r", "again"}, {"esc", "back"}, {"q", "quit"}};
    if (!rebuilt && !checked.drift.empty()) {
        hints.insert(hints.begin(), {{glyph.move, "move"},
                                     {glyph.enter, "open"},
                                     {"u", app.update() == Update::save ? "rebuild" : "preview"}});
    }
    draw_hints(screen, size.rows - 1, size.cols, hints);
}

// text repeated count times.
[[nodiscard]] std::string repeat(std::string_view text, std::size_t count);

// A dialog centred over the view: its title in the top edge, its lines inside a margin (cut to
// fit), and how to close it in the bottom edge.
template <class S>
void draw_dialog(S& screen, const Dialog& dialog, const Glyphs& glyph, Size size) {
    const Pen edge{.fg = palette::overlay, .bg = palette::mantle};
    const Pen body{.fg = palette::text, .bg = palette::mantle};
    auto title_pen = tone_pen(dialog.error ? Tone::bad : Tone::heading);
    title_pen.bg = palette::mantle;
    title_pen.bold = true;
    const std::string closing = " any key ";
    std::string title = dialog.error ? std::format(" {} {} ", glyph.broken, dialog.title)
                                     : std::format(" {} ", dialog.title);
    std::size_t widest = columns(title) + columns(closing);
    for (const auto& line : dialog.lines) {
        widest = std::max(widest, columns(line) + 4);
    }
    const auto width = std::min<std::size_t>(widest + 2, size.cols - 2);
    const auto inner = width - 2;
    const auto shown = std::min<std::size_t>(dialog.lines.size(), size.rows - 4);
    const auto height = shown + 4;
    const auto top = static_cast<unsigned>((size.rows - height) / 2);
    const auto left = static_cast<unsigned>((size.cols - width) / 2);
    const auto right = static_cast<unsigned>(left + width);

    title = clip(title, inner - 1);
    put_spans(screen, top, left,
              {{std::format("{}{}", glyph.frame.top_left, glyph.frame.across), edge},
               {title, title_pen},
               {std::format("{}{}", repeat(glyph.frame.across, inner - 1 - columns(title)),
                            glyph.frame.top_right),
                edge}},
              right);
    const auto blank = [&](unsigned row, std::string_view text) {
        const auto cut = clip(text, inner - 4);
        put_spans(screen, row, left,
                  {{std::string{glyph.frame.down}, edge},
                   {std::format("  {}{}", cut, std::string(inner - 2 - columns(cut), ' ')), body},
                   {std::string{glyph.frame.down}, edge}},
                  right);
    };
    blank(top + 1, "");
    for (std::size_t line = 0; line < shown; ++line) {
        blank(top + 2 + static_cast<unsigned>(line), dialog.lines.at(line));
    }
    blank(static_cast<unsigned>(top + height - 2), "");
    const auto key_room = std::min(columns(closing), inner - 1);
    put_spans(
        screen, static_cast<unsigned>(top + height - 1), left,
        {{std::format("{}{}", glyph.frame.bottom_left,
                      repeat(glyph.frame.across, inner - 1 - key_room)),
          edge},
         {clip(closing, key_room), {.fg = palette::mauve, .bg = palette::mantle, .bold = true}},
         {std::format("{}{}", glyph.frame.across, glyph.frame.bottom_right), edge}},
        right);
}

// "45s", "1m40s", "1h02m": seconds, rounded down, in at most two units.
[[nodiscard]] std::string duration(double seconds);
// "512 B", "1.5 KiB", "2.0 MiB".
[[nodiscard]] std::string byte_size(std::uint64_t bytes);
// done of total as width cells: full ones, one filled by eighths where the glyphs have them, then
// empty ones.
[[nodiscard]] std::string progress_bar(std::uint64_t done, std::uint64_t total, std::size_t width,
                                       const Glyphs& glyph);
// The spinner's frame for a count, cycling.
[[nodiscard]] std::string spinner_frame(std::size_t count, const Glyphs& glyph);

// What a task is doing, in words.
[[nodiscard]] std::string task_state(const emerge::Task& task);

// A task's row after the tree: a spinner while it runs, its kind, cpv, state, elapsed time, and
// its cgroup's CPU parallelism and peak memory when reported.
inline std::vector<Span> task_spans(const emerge::Task& task, std::size_t frame,
                                    const Glyphs& glyph) {
    const bool waiting = task.merge_wait;
    const auto kind = waiting                                ? glyph.waiting
                      : task.kind == emerge::TaskKind::merge ? glyph.merge
                      : task.binary                          ? glyph.binary
                                                             : glyph.build;
    std::vector<Span> spans{
        {waiting ? std::string{" "} : spinner_frame(frame, glyph), tone_pen(Tone::heading)},
        {std::format(" {} ", kind), tone_pen(waiting ? Tone::note : Tone::build)}};
    std::ranges::move(cpv_spans(task.cpv), std::back_inserter(spans));
    const auto used = columns(task.cpv);
    spans.push_back({std::string(used < 44 ? 46 - used : 2, ' '), {}});
    const auto state = task_state(task);
    spans.push_back({std::format("{:<18}", state), tone_pen(waiting ? Tone::note : Tone::choice)});
    // A waiting task's own time stopped when its build did.
    const auto elapsed = waiting && task.build_elapsed ? task.build_elapsed : task.elapsed;
    spans.push_back({std::format("{:>7}", elapsed ? duration(*elapsed) : std::string{}),
                     tone_pen(Tone::version)});
    if (task.resources.cpu_usec && task.build_elapsed && *task.build_elapsed > 0) {
        const auto busy = static_cast<double>(*task.resources.cpu_usec) / 1e6 / *task.build_elapsed;
        spans.push_back({std::format("   {:.1f}x CPU", busy), tone_pen(Tone::note)});
    }
    if (task.resources.mem_peak) {
        spans.push_back(
            {std::format("   {} peak", byte_size(*task.resources.mem_peak)), tone_pen(Tone::note)});
    }
    return spans;
}

// An emerge's heading: its pid, jobs, and progress as a bar and a percentage.
inline std::vector<Span> emerge_spans(const emerge::Snapshot& snapshot, const Glyphs& glyph) {
    const auto& jobs = snapshot.jobs;
    const auto percent = jobs.total == 0 ? 0 : jobs.completed * 100 / jobs.total;
    std::vector<Span> spans{
        {std::format(" {} emerge {}", glyph.package, snapshot.pid), tone_pen(Tone::heading)},
        {jobs.max ? std::format("   {} of {} jobs", jobs.running, *jobs.max)
                  : std::format("   {} jobs", jobs.running),
         tone_pen(Tone::note)},
        {std::format("   {} of {} done   ", jobs.completed, jobs.total), tone_pen(Tone::note)},
        {progress_bar(jobs.completed, jobs.total, 20, glyph), tone_pen(Tone::good)},
        {std::format(" {:>3}%", percent), tone_pen(Tone::heading)}};
    if (jobs.failed > 0) {
        spans.push_back(
            {std::format("   {} {} failed", glyph.broken, jobs.failed), tone_pen(Tone::bad)});
    }
    return spans;
}

// The last width values as a sparkline scaled to max, right-aligned. Cells over limit are in the
// bad tone, the rest in tone; a missing value is a blank cell.
[[nodiscard]] std::vector<Span> sparkline(const std::vector<std::optional<double>>& values,
                                          double max, std::size_t width, const Glyphs& glyph,
                                          Tone tone, std::optional<double> limit = std::nullopt);

// Rows the pressure panel takes: a heading and a graph each for CPU, memory, load and stalls.
inline constexpr unsigned pressure_rows = 5;

// The pressure panel from row down: what the history holds, with steve's limits marked.
[[nodiscard]] std::vector<std::vector<Span>> pressure_lines(const pressure::History& history,
                                                            const Limits& limits, std::size_t width,
                                                            const Glyphs& glyph);

template <class S> void draw_watch(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& open = app.watched();
    if (!open) {
        return;
    }
    const auto& watched = *open;
    draw_title(screen, app, size.cols,
               {{std::format(" {} egraph ", glyph.package),
                 {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                {std::format(" {} ", glyph.trail), tone_pen(Tone::note)},
                {"emerge", tone_pen(Tone::name)}});
    const unsigned first = 2;
    // The pressure panel sits above the hints, a blank line above it, where there is room.
    const bool panel = size.rows >= first + pressure_rows + 6;
    const unsigned height = size.rows - first - 1 - (panel ? pressure_rows + 1 : 0);
    app.set_height(height);
    if (panel) {
        const auto graph = std::clamp<std::size_t>(size.cols > 60 ? size.cols - 60 : 0, 8, 120);
        unsigned row = size.rows - 1 - pressure_rows;
        for (const auto& spans : pressure_lines(watched.history, watched.limits, graph, glyph)) {
            put_spans(screen, row++, 0, spans, size.cols);
        }
    }
    if (watched.snapshots.empty()) {
        put_spans(screen, 1, 1, {{"No emerge is publishing its progress", tone_pen(Tone::heading)}},
                  size.cols);
        put_spans(screen, 3, 3,
                  {{"emerge publishes it to /run/portage with FEATURES=\"observability\";",
                    tone_pen(Tone::note)}},
                  size.cols);
        put_spans(screen, 4, 3, {{"this view reads it again every second.", tone_pen(Tone::note)}},
                  size.cols);
        draw_hints(screen, size.rows - 1, size.cols, {{"esc", "back"}, {"q", "quit"}});
        return;
    }
    // Every line, with the selected one's index, then a window that keeps it in view.
    struct Line {
        std::vector<Span> spans;
        bool selected = false;
    };
    std::vector<Line> lines;
    std::size_t task_index = 0;
    std::size_t selected_line = 0;
    for (const auto& snapshot : watched.snapshots) {
        if (!lines.empty()) {
            lines.push_back({});
        }
        lines.push_back({.spans = emerge_spans(snapshot, glyph)});
        for (std::size_t at = 0; at < snapshot.tasks.size(); ++at) {
            const bool selected = task_index++ == watched.cursor.at;
            if (selected) {
                selected_line = lines.size();
            }
            std::vector<Span> spans{
                marker(selected, glyph),
                {std::format("  {} ", at + 1 == snapshot.tasks.size() ? glyph.branch : glyph.tee),
                 tone_pen(Tone::note)}};
            std::ranges::move(task_spans(snapshot.tasks.at(at), watched.frame, glyph),
                              std::back_inserter(spans));
            lines.push_back({.spans = std::move(spans), .selected = selected});
        }
    }
    const auto top = selected_line >= height ? selected_line - height + 1 : 0;
    for (unsigned line = 0; line < height && top + line < lines.size(); ++line) {
        const auto& shown = lines.at(top + line);
        const auto bg =
            shown.selected ? std::optional<Color>{palette::surface} : std::optional<Color>{};
        if (shown.selected) {
            screen.fill_row(first + line, {.fg = std::nullopt, .bg = palette::surface});
        }
        put_spans(screen, first + line, 0, shown.spans, size.cols, bg);
    }
    draw_hints(screen, size.rows - 1, size.cols,
               {{glyph.move, "move"}, {glyph.enter, "open"}, {"esc", "back"}, {"q", "quit"}});
}

template <class S> void draw(S& screen, App& app, const Glyphs& glyph) {
    const auto size = screen.size();
    screen.clear();
    if (size.rows >= 5 && size.cols >= 10) {
        if (!app.pages().empty()) {
            draw_page(screen, app, glyph, size);
        } else if (app.checked()) {
            draw_check(screen, app, glyph, size);
        } else if (app.watched()) {
            draw_watch(screen, app, glyph, size);
        } else {
            draw_list(screen, app, glyph, size);
        }
        if (app.dialog()) {
            draw_dialog(screen, *app.dialog(), glyph, size);
        }
    }
    screen.render();
}

// Runs until the user quits or input ends, making the fresh build a check asks for, or the
// rebuild, once the waiting view is on screen.
template <class S> void run(S& screen, App& app, const Glyphs& glyph, const Services& services) {
    draw(screen, app, glyph);
    while (!app.done()) {
        if (app.check_requested()) {
            app.finish_check(services.check ? services.check(app.store())
                                            : std::unexpected(std::string{"no way to check"}));
        } else if (app.rebuild_requested()) {
            app.finish_rebuild(services.rebuild
                                   ? services.rebuild()
                                   : std::unexpected(std::string{"no way to rebuild"}));
        } else if (app.watch_requested()) {
            app.finish_watch(services.watch ? services.watch() : std::vector<emerge::Snapshot>{},
                             services.sample ? services.sample() : pressure::Sample{});
        } else {
            app.handle(screen.read(app.refresh()));
        }
        if (!app.done()) {
            draw(screen, app, glyph);
        }
    }
}

// Opens the terminal and runs the interface over store, first showing any warnings from opening
// it; errors go to err.
[[nodiscard]] Exit open_and_run(const Store& store, GlyphSet glyphs, const Services& services,
                                std::span<const std::string> warnings, std::ostream& err);

} // namespace egraph::tui
