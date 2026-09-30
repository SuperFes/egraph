#pragma once

// The terminal interface. App is the state and what keys do to it, with no terminal in sight;
// draw() and run() are templates over the screen, so tests drive them with a fake one and only
// tui.cpp pairs them with the Notcurses Screen.

#include "cli.hpp"
#include "depclean.hpp"
#include "emerge.hpp"
#include "evaluated.hpp"
#include "graph.hpp"
#include "human.hpp"
#include "job.hpp"
#include "plan.hpp"
#include "pressure.hpp"
#include "query.hpp"
#include "remedy.hpp"
#include "screen.hpp"
#include "steve.hpp"
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
// package is now installed at another version or slot. An update row is what emerge -u would do
// to the page package, a held row the update the plan holds back; an unmatched row a dependency
// the ebuild would add with flags toggled that nothing installed satisfies, its atom in
// link.atom. A remedy row is a line of the commands past a held update.
enum class RowType : std::uint8_t {
    heading,
    note,
    alert,
    missing,
    replaced,
    link,
    root,
    path,
    update,
    held,
    unmatched,
    remedy
};

// A page row. Links at depth 0 are the page package's own; unfolding a link puts its links,
// in the same direction, one level deeper right below it.
struct Row {
    RowType type = RowType::note;
    // What a text row says; for a link or unmatched row the ebuild would add, its atom, which is
    // the evaluated store's string and so not link.atom.
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
    // For links and unmatched rows the ebuild would add: the flags to toggle, "+flag" or "-flag".
    std::string flags;
    // For an update or held row; an update row's text names the merge a slot-operator rebuild
    // is for.
    std::optional<PendingUpdate> update;
    // For a remedy row, its label padded to the others'.
    std::optional<RemedyLine> remedy;
};

// Sets each row's last and rails, walking up from the bottom: a level's line continues past a
// row when a sibling at that level follows before anything shallower does. depth_of gives a
// row's depth, 0 for one outside the tree.
template <class T, class Depth> void thread_tree(std::vector<T>& rows, Depth depth_of) {
    std::vector<bool> later;
    for (auto& row : std::ranges::reverse_view(rows)) {
        const std::size_t depth = depth_of(row);
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

void thread(std::vector<Row>& rows);

// A scrolling list's selected entry and first visible one.
struct Cursor {
    std::size_t at = 0;
    std::size_t top = 0;
};

// A fresh build, and how the installed store differs from it: drift lines ("+cpv", "-cpv",
// "~cpv"). The evaluated store is empty where there is none.
struct Fresh {
    Store store;
    Evaluated evaluated{};
    std::vector<std::string> drift;
};
// The spinner's frame for a count, cycling.
[[nodiscard]] std::string spinner_frame(std::size_t count, const Glyphs& glyph);

// What `egraph check` finds, or why it could not run.
using CheckResult = std::expected<Fresh, std::string>;
// Builds a fresh store and compares the given installed store with it, which outlives the job.
using Checker = std::function<Job<CheckResult>(const Store&)>;
using RebuildResult = std::expected<std::shared_ptr<const Stores>, std::string>;
// Writes fresh stores over the ones on disk and loads them, or says why it could not.
using Rebuilder = std::function<Job<RebuildResult>()>;

// Why stores no longer describe the system, or nothing while they do.
using Staleness = std::function<std::optional<std::string>(const Stores&)>;
using RefreshResult = std::expected<std::shared_ptr<const Stores>, std::string>;
// Brings the stores up to date: the job ends with current ones, or why it could not.
using Refresher = std::function<Job<RefreshResult>()>;
// The time, which says when to look at the stores' inputs again.
using Clock = std::function<std::chrono::steady_clock::time_point()>;

// How often the stores' inputs are looked at, and how long a failed refresh waits to try again.
inline constexpr std::chrono::milliseconds stale_interval{2000};
inline constexpr std::chrono::milliseconds retry_interval{60000};

// Reads the running emerges' snapshots.
using Watcher = std::function<std::vector<emerge::Snapshot>()>;

// Reads /proc for the pressure graphs.
using Sampler = std::function<pressure::Sample()>;

// The running steve, or nothing when none runs.
using SteveReader = std::function<std::optional<steve::Status>()>;
// Changes one of steve's settings.
using SteveSetter = std::function<std::expected<void, std::string>(steve::Setting, double)>;

// The running emerge's merge list, and what each of its packages waits for.
using MergeListReader = std::function<std::vector<emerge::Pending>()>;
using Planner =
    std::function<std::expected<emerge::Waits, std::string>(const std::vector<emerge::Pending>&)>;

// What a command typed at the : prompt printed: its output in the lines layout, its errors and
// warnings, and how it ended.
struct Answer {
    Exit exit = Exit::ok;
    std::string out;
    std::string err;
    // The command was quit, which ends the interface.
    bool quit = false;
};
// Runs a command line, as `egraph shell` runs one.
using Commander = std::function<Answer(const std::string&)>;

// What the interface asks of the world outside it: run() calls these, the app never does.
struct Services {
    Checker check{};
    // Empty where the store cannot be written, so that a check's fresh build is only previewed.
    Rebuilder rebuild{};
    Watcher watch{};
    Sampler sample{};
    SteveReader steve{};
    SteveSetter set_steve{};
    MergeListReader merge_list{};
    Planner plan{};
    Commander command{};
    // Empty where the stores are not to be refreshed.
    Staleness stale{};
    Refresher refresh{};
    // The steady clock when empty.
    Clock now{};
};

struct Watched;

// A line under an emerge in its view: a running task, or a package on its merge list placed
// under what it waits for.
struct WatchRow {
    std::size_t snapshot = 0;
    std::string cpv{};
    // Running now.
    std::optional<emerge::Task> task{};
    // For a merge list package: how many others it waits for.
    std::optional<std::size_t> waiting_for{};
    bool binary = false;
    std::size_t depth = 1;
    bool last = true;
    std::vector<bool> rails{};
};

// The rows under each emerge, in order. The merge list belongs to the emerge running most of
// its packages (the only one, if there is one); its tasks not on the list come first.
[[nodiscard]] std::vector<WatchRow> watch_rows(const Watched& watched);

// Where steve stops handing out jobs, marked on the pressure graphs when known.
struct Limits {
    std::optional<double> load{};
    std::optional<std::uint64_t> min_available{};
};

[[nodiscard]] Limits limits_of(const std::optional<steve::Status>& steve);

// A change to one of steve's settings, waiting for run() to make it.
struct SteveChange {
    steve::Setting setting = steve::Setting::jobs;
    double value = 0;
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
    std::optional<steve::Status> steve;
    // The setting being changed, as an index into steve::all_settings.
    std::optional<std::size_t> editing;
    std::optional<SteveChange> change;
    std::vector<emerge::Pending> merge_list;
    emerge::Waits waits;
    // The cpvs last asked of the planner, so a failure is not asked again every second.
    std::vector<std::string> planned;
    std::optional<std::string> plan_error;
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
    std::optional<Stores> fresh;
    Cursor cursor;
    // Ticks while waiting, which turn the spinner.
    std::size_t frame = 0;
};

// How often the check view polls a build it waits for.
inline constexpr std::chrono::milliseconds wait_interval{100};

// A message over whatever is on screen, which the next key dismisses.
struct Dialog {
    bool error = true;
    std::string title;
    std::vector<std::string> lines;
};

// A line of the plan view: a root set at depth 0, then the packages down the chain from it to
// each merge, as updates --tree draws them.
struct PlanRow {
    // The set ("@selected", empty for merges nothing keeps), or a package's cpv.
    std::string label;
    // Index into Plan::merges, for a merge; the packages between merges are installed ones.
    std::optional<std::uint32_t> merge;
    std::size_t depth = 0;
    bool last = false;
    std::vector<bool> rails;
};

// The plan as a tree under each root set, chains as update_tree_lines finds them with kept.
[[nodiscard]] std::vector<PlanRow> plan_rows(const Store& store, const Evaluated& evaluated,
                                             const Kept& kept, const Plan& plan);

// The plan view: its rows, each merge's place in the merge order (from 1), and the cursor over
// the packages.
struct Planned {
    std::vector<PlanRow> rows;
    std::vector<std::size_t> places;
    Cursor cursor;
};

// Which packages the list shows, before the search.
enum class Only : std::uint8_t { all, orphans, broken, updates };

// The rebuilds for USE the interface shows beside replacements: all of --newuse's.
inline constexpr UseRebuilds shown_rebuilds = UseRebuilds::all;

class App {
  public:
    // The installed store alone: its own dependencies, every package unmasked, no updates.
    App(const Store& store, const Graph& graph, Update update = Update::preview);
    // Both stores, the dependencies read as emerge reads them with dynamic_deps or without.
    App(std::shared_ptr<const Stores> stores, bool dynamic_deps, Update update = Update::preview);
    App(Stores stores, bool dynamic_deps, Update update = Update::preview);

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

    // The store the queries read: the installed one, with dynamic deps its trees the evaluated's.
    [[nodiscard]] const Store& store() const { return store_.get(); }
    [[nodiscard]] const Store& installed() const { return installed_.get(); }
    // Empty without an evaluated store.
    [[nodiscard]] const Evaluated& evaluated() const { return evaluated_.get(); }
    [[nodiscard]] bool has_evaluated() const {
        return !evaluated().packages.empty() &&
               evaluated().packages.size() == installed().packages.size();
    }
    // What emerge -uD would merge for the package, as the plan weighs it.
    [[nodiscard]] const std::optional<PendingUpdate>& update_of(std::uint32_t package) const {
        return updates_.at(package);
    }
    // The package's update the plan holds back, as an index into plan().held and remedies().
    [[nodiscard]] std::optional<std::uint32_t> held_of(std::uint32_t package) const {
        return held_.at(package);
    }
    [[nodiscard]] const Plan& plan() const { return plan_; }
    [[nodiscard]] const std::vector<Remedy>& remedies() const { return remedies_; }
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
    void finish_rebuild(RebuildResult result);
    // Open from the list, under any pages opened from it.
    [[nodiscard]] const std::optional<Watched>& watched() const { return watched_; }
    // Open from the list, under any pages opened from it.
    [[nodiscard]] const std::optional<Planned>& planned() const { return planned_; }
    // Whether the emerge view is showing and due to read the snapshots, which run() then does.
    [[nodiscard]] bool watch_requested() const;
    void finish_watch(std::vector<emerge::Snapshot> snapshots, const pressure::Sample& sample = {},
                      std::optional<steve::Status> steve = std::nullopt,
                      std::vector<emerge::Pending> merge_list = {});
    // The merge list, when some of its packages' waits are not known yet.
    [[nodiscard]] std::optional<std::vector<emerge::Pending>> plan_requested() const;
    void finish_plan(std::expected<emerge::Waits, std::string> result);
    // A change to steve waiting for run() to make it.
    [[nodiscard]] std::optional<SteveChange> steve_change_requested() const;
    void finish_steve_change(const std::expected<void, std::string>& result);
    // The stores shown, shared; empty for an app over an installed store alone.
    [[nodiscard]] std::shared_ptr<const Stores> shared() const;
    // Whether the stores shown still describe the system: a reason asks for a refresh.
    void finish_stale_check(std::optional<std::string> reason);
    // Whether a refresh is asked for, and not yet made.
    [[nodiscard]] bool stale() const { return stale_.has_value(); }
    // Whether run() is to make the refresh asked for, in the background: not while the check
    // view is open, since its build compares with the stores shown.
    [[nodiscard]] bool refresh_requested() const { return stale_ && !checked_; }
    // Shows refreshed stores in place of the current ones, each view where it was.
    void finish_refresh(RefreshResult result);
    // Why the last refresh failed, until one succeeds.
    [[nodiscard]] const std::optional<std::string>& refresh_error() const { return refresh_error_; }
    // Ticks while refreshing, which turn the spinner.
    [[nodiscard]] std::size_t frame() const { return frame_; }
    // How long run() waits for a key before the view needs drawing again; unset waits for one.
    [[nodiscard]] std::optional<std::chrono::milliseconds> refresh() const;
    // The : prompt's text while the user types a command.
    [[nodiscard]] const std::optional<std::string>& prompt() const { return prompt_; }
    // A command typed at the prompt, waiting for run() to answer it.
    [[nodiscard]] const std::optional<std::string>& command_requested() const { return command_; }
    void finish_command(const Answer& answer);
    // What the last command printed: rows of its tab-separated fields, each linked to the first
    // installed package a field names.
    struct Output {
        std::string command;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::optional<std::uint32_t>> links;
        Cursor cursor;
    };
    // Over the list, the check view and the emerge view, under pages opened from it.
    [[nodiscard]] const std::optional<Output>& output() const { return output_; }
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
    // Stores the app owns, the one its queries read, and that one's graph.
    struct Loaded {
        Loaded(std::shared_ptr<const Stores> shared, bool dynamic_deps);
        std::shared_ptr<const Stores> stores;
        // With dynamic deps; the installed store is read otherwise.
        std::optional<Store> dynamic;
        Graph graph;
    };

    void index();
    // Shows the stores in place of the current ones, back at the list (or the check view).
    void adopt(std::shared_ptr<const Stores> stores, Source source);
    // As adopt, each view kept where it was: its package found again by cpv, or else by name
    // and slot.
    void replace(std::shared_ptr<const Stores> stores);
    // The package in the stores shown that stands for cpv, of cp and slot, from other stores.
    [[nodiscard]] std::optional<std::uint32_t> find_again(std::string_view cpv, std::string_view cp,
                                                          std::string_view slot) const;
    // The first installed package a command's output fields name.
    [[nodiscard]] std::optional<std::uint32_t>
    link_of(const std::vector<std::string>& fields) const;
    void own(std::unique_ptr<const Loaded> loaded);
    void filter();
    void recompute();
    void open(std::uint32_t package);
    void unfold(Page& page);
    void fold(Page& page);
    void handle_list(const Key& key);
    void handle_page(const Key& key);
    void handle_check(const Key& key);
    void handle_watch(const Key& key);
    void handle_steve(const Key& key);
    void handle_plan(const Key& key);
    // The plan view over the current plan, on the row labelled selected where there is one.
    Planned& open_plan(std::optional<std::string> selected = std::nullopt);
    void handle_prompt(std::string& text, const Key& key);
    void handle_output(Output& output, const Key& key);
    // The package with this cpv, if the store has it.
    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view cpv) const;

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Store> installed_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::reference_wrapper<const Graph> graph_;
    // Behind a pointer so the references stay valid when the app moves.
    std::unique_ptr<const Loaded> owned_;
    bool dynamic_deps_ = false;
    Update update_;
    Source source_ = Source::opened;
    std::vector<std::string> folded_;
    std::vector<std::size_t> dependencies_;
    std::vector<std::size_t> dependents_;
    std::vector<std::size_t> broken_;
    std::vector<std::size_t> broken_at_run_time_;
    std::vector<Masking> masking_;
    std::vector<std::optional<PendingUpdate>> updates_;
    // Per package, what the merge a slot-operator rebuild is for; empty otherwise.
    std::vector<std::string> rebuilt_for_;
    Plan plan_;
    std::vector<Remedy> remedies_;
    std::vector<std::optional<std::uint32_t>> held_;
    bool build_deps_ = true;
    Kept kept_;
    std::vector<std::optional<std::uint32_t>> root_of_;
    List list_;
    std::optional<Checked> checked_;
    std::optional<Watched> watched_;
    std::optional<Planned> planned_;
    std::vector<Page> pages_;
    std::optional<Dialog> dialog_;
    std::optional<std::string> prompt_;
    std::optional<std::string> command_;
    std::optional<Output> output_;
    std::optional<std::string> stale_;
    std::optional<std::string> refresh_error_;
    std::size_t frame_ = 0;
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

// The trail, and on the right a refresh under way or failed, and a warning while a fresh build
// is shown without being saved.
template <class S>
void draw_title(S& screen, const App& app, unsigned width, const std::vector<Span>& trail,
                const Glyphs& glyph) {
    screen.fill_row(0, {.fg = palette::text, .bg = palette::crust});
    put_spans(screen, 0, 0, trail, width, palette::crust);
    std::vector<Span> badges;
    if (app.refresh_requested()) {
        badges.push_back({std::format(" {} refreshing ", spinner_frame(app.frame(), glyph)),
                          {.fg = palette::overlay, .bg = palette::crust}});
    } else if (const auto& error = app.refresh_error()) {
        // The first line; the builder's own output follows it.
        auto first = error->substr(0, error->find('\n'));
        if (first.ends_with(':')) {
            first.pop_back();
        }
        auto pen = tone_pen(Tone::bad);
        pen.bg = palette::crust;
        badges.push_back({std::format(" refresh failed: {} ", first), pen});
    }
    if (app.source() == Source::preview) {
        badges.push_back(
            {" preview, not saved ", {.fg = palette::crust, .bg = palette::mauve, .bold = true}});
    }
    std::size_t used = 0;
    for (const auto& badge : badges) {
        used += columns(badge.text);
    }
    if (used > 0 && used < width) {
        put_spans(screen, 0, width - static_cast<unsigned>(used), badges, width);
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
    switch (list.query.empty() ? list.only : Only::all) {
    case Only::all:
        return "no package matches";
    case Only::orphans:
        return "nothing to remove";
    case Only::broken:
        return "nothing broken";
    case Only::updates:
        return "nothing to update";
    }
    return "no package matches";
}

// An update's glyph, in its tone.
inline Span update_glyph(const PendingUpdate& update, const Glyphs& glyph) {
    switch (update.kind) {
    case UpdateKind::upgrade:
        return {std::string{glyph.upgrade}, tone_pen(Tone::good)};
    case UpdateKind::downgrade:
        return {std::string{glyph.downgrade}, tone_pen(Tone::bad)};
    case UpdateKind::rebuild:
        return {std::string{glyph.rebuild}, tone_pen(Tone::use)};
    }
    return {std::string{glyph.rebuild}, tone_pen(Tone::use)};
}

// A pending update after a package in the list: the version it moves to, or that it is rebuilt.
// A held one says so, after any version it falls back to.
inline std::vector<Span> update_mark(const App& app, std::uint32_t package, const Glyphs& glyph) {
    const auto& update = app.update_of(package);
    const auto held = app.held_of(package);
    std::vector<Span> spans;
    if (update) {
        const auto& evaluated = app.evaluated();
        const auto version =
            split_cpv(evaluated.string(evaluated.candidates.at(update->target).cpv));
        spans = {
            {"  ", {}},
            update_glyph(*update, glyph),
            update->kind == UpdateKind::rebuild
                ? Span{" rebuild", tone_pen(Tone::use)}
                : Span{std::format(" {}", version.version),
                       tone_pen(update->kind == UpdateKind::upgrade ? Tone::good : Tone::bad)}};
    }
    if (held) {
        spans.push_back({"  ", {}});
        spans.push_back({std::format("{} held", glyph.held), tone_pen(Tone::bad)});
    }
    return spans;
}

// A USE rebuild's flags as emerge shows them, those whose state changed in the use tone.
inline std::vector<Span> rebuild_flags(std::string_view flags) {
    std::vector<Span> spans;
    for (const auto flag : std::views::split(flags, ' ')) {
        const std::string_view text{flag};
        spans.push_back(
            {std::format(" {}", text), tone_pen(text.contains('*') ? Tone::use : Tone::note)});
    }
    return spans;
}

template <class S> void draw_list(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& list = app.list();
    const auto& store = app.store();
    std::vector<Span> title{{std::format(" {} egraph ", glyph.package),
                             {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                            {std::format(" {}  ", store.meta.eroot), tone_pen(Tone::note)}};
    if (list.only == Only::orphans) {
        title.push_back({std::format("{} orphans", list.shown.size()), tone_pen(Tone::count)});
    } else if (list.only == Only::updates) {
        title.push_back({std::format("{} updates", list.shown.size()), tone_pen(Tone::count)});
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
    draw_title(screen, app, size.cols, title, glyph);

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
        std::ranges::move(update_mark(app, id, glyph), std::back_inserter(spans));
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
        std::vector<std::pair<std::string_view, std::string_view>> hints{
            {glyph.move, "move"},
            {glyph.enter, "open"},
            {"/", "search"},
            {"o", list.only == Only::orphans ? "all" : "orphans"},
            {"!", list.only == Only::broken ? "all" : "broken"}};
        if (app.has_evaluated()) {
            hints.emplace_back("u", list.only == Only::updates ? "all" : "updates");
            hints.emplace_back("p", "plan");
        }
        hints.insert(hints.end(), {{"b", app.build_deps() ? "run time only" : "build deps"},
                                   {"c", "check"},
                                   {"e", "emerges"},
                                   {"q", "quit"},
                                   {":", "command"}});
        draw_hints(screen, size.rows - 1, size.cols, hints);
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
    draw_title(screen, app, size.cols, trail, glyph);
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
        case RowType::update:
        case RowType::held: {
            if (!row.update) {
                break;
            }
            const auto& update = *row.update;
            const auto& target = app.evaluated().candidates.at(update.target);
            std::vector<Span> spans{{"   ", {}},
                                    row.type == RowType::held
                                        ? Span{std::string{glyph.held}, tone_pen(Tone::bad)}
                                        : update_glyph(update, glyph)};
            if (update.kind == UpdateKind::rebuild && !row.text.empty()) {
                spans.push_back({" rebuild for ", tone_pen(Tone::use)});
                spans.push_back({row.text, tone_pen(Tone::note)});
            } else if (update.kind == UpdateKind::rebuild) {
                spans.push_back({update.flags.empty() ? " rebuild, its own ebuild being masked"
                                                      : " rebuild for",
                                 tone_pen(Tone::use)});
                std::ranges::move(rebuild_flags(update.flags), std::back_inserter(spans));
            } else {
                spans.push_back(
                    {update.kind == UpdateKind::upgrade ? " upgrade to " : " downgrade to ",
                     tone_pen(Tone::note)});
                std::ranges::move(cpv_spans(app.evaluated().string(target.cpv)),
                                  std::back_inserter(spans));
            }
            spans.push_back(
                {std::format("  ::{}", app.evaluated().string(target.repo)), tone_pen(Tone::repo)});
            put_spans(screen, at, 0, spans, size.cols);
            break;
        }
        case RowType::remedy:
            if (row.remedy) {
                put_spans(screen, at, 3,
                          {{row.remedy->label, tone_pen(Tone::heading)},
                           {row.remedy->text, tone_pen(row.remedy->tone)}},
                          size.cols);
            }
            break;
        case RowType::unmatched: {
            std::vector<Span> spans{{"   ", {}}};
            for (std::size_t k = 0; k < kind_shorthands.size(); ++k) {
                const auto& kind = kind_shorthands.at(k);
                spans.push_back(row.link.kinds.at(k)
                                    ? Span{std::string{kind.letter}, tone_pen(kind.tone)}
                                    : Span{std::string{glyph.absent}, tone_pen(Tone::note)});
            }
            spans.push_back({"   ", {}});
            spans.push_back({row.text, tone_pen(Tone::note)});
            if (row.link.choice) {
                spans.push_back({std::format(" {}", glyph.choice), tone_pen(Tone::choice)});
            }
            spans.push_back({std::format("  {}", row.flags), tone_pen(Tone::use)});
            spans.push_back({"  not installed", tone_pen(Tone::note)});
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
            // A link the ebuild would add carries its atom as text.
            spans.push_back({row.text.empty() ? std::string{store.string(row.link.atom)} : row.text,
                             tone_pen(Tone::note)});
            if (row.link.choice) {
                spans.push_back({std::format(" {}", glyph.choice), tone_pen(Tone::choice)});
            }
            if (!row.flags.empty()) {
                spans.push_back({std::format("  {}", row.flags), tone_pen(Tone::use)});
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
                {"check", tone_pen(Tone::name)}},
               glyph);
    const unsigned first = 3;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    using Stage = Checked::Stage;
    if (checked.stage == Stage::checking || checked.stage == Stage::rebuilding) {
        put_spans(screen, 1, 1,
                  {{spinner_frame(checked.frame, glyph), tone_pen(Tone::heading)},
                   {checked.stage == Stage::checking
                        ? " Building a fresh store to compare with; this takes a few seconds"
                        : " Rebuilding the store; this takes a few seconds",
                    tone_pen(Tone::note)}},
                  size.cols);
        draw_hints(screen, size.rows - 1, size.cols, {{"esc", "stop"}, {"q", "quit"}});
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

// A merge in the plan view, as updates --tree draws it: its kind, name, version and any it moves
// to, repository, place, and the places of the merges it waits for.
inline std::vector<Span> merge_spans(const App& app, const Planned& planned, std::uint32_t index,
                                     const Glyphs& glyph) {
    const auto& merge = app.plan().merges.at(index);
    const auto& evaluated = app.evaluated();
    const auto& target = evaluated.candidates.at(merge.candidate);
    const auto to = split_cpv(evaluated.string(target.cpv));
    const auto from =
        merge.replaces ? split_cpv(app.store().string(app.store().packages.at(*merge.replaces).cpv))
                       : to;
    std::vector<Span> spans;
    if (merge.replaces) {
        spans.push_back(update_glyph(
            {.kind = merge.kind, .target = merge.candidate, .flags = merge.flags}, glyph));
    } else {
        spans.push_back({std::string{glyph.added}, tone_pen(Tone::good)});
    }
    spans.push_back({" ", {}});
    spans.push_back({std::format("{}/", from.category), tone_pen(Tone::category)});
    spans.push_back({std::string{from.name}, tone_pen(Tone::name)});
    spans.push_back({std::format("  {}", from.version), tone_pen(Tone::version)});
    if (from.version != to.version) {
        spans.push_back({std::format(" {} ", glyph.instead), tone_pen(Tone::note)});
        spans.push_back({std::string{to.version},
                         tone_pen(merge.kind == UpdateKind::downgrade ? Tone::bad : Tone::good)});
    }
    spans.push_back({std::format("  ::{}", evaluated.string(target.repo)), tone_pen(Tone::repo)});
    spans.push_back({std::format("  {}", planned.places.at(index)), tone_pen(Tone::count)});
    if (!merge.waits.empty()) {
        std::string waits;
        for (const auto wait : merge.waits) {
            waits += std::format("{}{}", waits.empty() ? "" : " ", planned.places.at(wait));
        }
        spans.push_back({std::format("  {} ", glyph.waiting), tone_pen(Tone::note)});
        spans.push_back({std::move(waits), tone_pen(Tone::count)});
    }
    return spans;
}

template <class S> void draw_plan(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& open = app.planned();
    if (!open) {
        return;
    }
    const auto& planned = *open;
    draw_title(screen, app, size.cols,
               {{std::format(" {} egraph ", glyph.package),
                 {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                {std::format(" {} ", glyph.trail), tone_pen(Tone::note)},
                {"plan", tone_pen(Tone::name)},
                {std::format("  {} merges", app.plan().merges.size()), tone_pen(Tone::count)}},
               glyph);
    const unsigned first = 2;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    for (unsigned line = 0; line < height; ++line) {
        const auto index = planned.cursor.top + line;
        if (index >= planned.rows.size()) {
            break;
        }
        const auto& row = planned.rows.at(index);
        const unsigned at = first + line;
        if (row.depth == 0) {
            put_spans(
                screen, at, 3,
                {row.label.empty()
                     ? Span{std::format("{} nothing keeps", glyph.orphan), tone_pen(Tone::bad)}
                     : Span{std::format("{} {}", set_glyph(row.label, glyph), row.label),
                            tone_pen(Tone::root)}},
                size.cols);
            continue;
        }
        const bool selected = index == planned.cursor.at;
        const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
        if (selected) {
            screen.fill_row(at, {.fg = std::nullopt, .bg = palette::surface});
        }
        std::string tree;
        for (const bool rail : row.rails) {
            tree += rail ? glyph.rail : "  ";
        }
        tree += std::format("{} ", row.last ? glyph.branch : glyph.tee);
        std::vector<Span> spans{marker(selected, glyph), {std::move(tree), tone_pen(Tone::note)}};
        std::ranges::move(row.merge ? merge_spans(app, planned, *row.merge, glyph)
                                    : cpv_spans(row.label),
                          std::back_inserter(spans));
        put_spans(screen, at, 0, spans, size.cols, bg);
    }
    if (planned.rows.empty()) {
        put_spans(screen, first, 5, {{"nothing to merge", tone_pen(Tone::note)}}, size.cols);
    }
    std::vector<std::pair<std::string_view, std::string_view>> hints{
        {"esc", "back"}, {"q", "quit"}, {":", "command"}};
    if (!planned.rows.empty()) {
        hints.insert(hints.begin(), {{glyph.move, "move"}, {glyph.enter, "open"}});
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

// A merge list package that is not running yet: its kind, cpv, and whether it can start.
inline std::vector<Span> row_spans(const WatchRow& row, std::size_t frame, const Glyphs& glyph) {
    if (row.task) {
        return task_spans(*row.task, frame, glyph);
    }
    const auto waiting_for = row.waiting_for.value_or(0);
    const bool ready = waiting_for == 0;
    std::vector<Span> spans{{std::format("  {} ", row.binary ? glyph.binary : glyph.queued),
                             tone_pen(ready ? Tone::good : Tone::note)}};
    std::ranges::move(cpv_spans(row.cpv), std::back_inserter(spans));
    const auto used = columns(row.cpv);
    spans.push_back({std::string(used < 44 ? 46 - used : 2, ' '), {}});
    spans.push_back(ready ? Span{"ready", tone_pen(Tone::good)}
                          : Span{std::format("waits for {}", waiting_for), tone_pen(Tone::note)});
    return spans;
}

// Which emerge the merge list belongs to, as watch_rows decides.
[[nodiscard]] std::size_t rows_owner(const Watched& watched);

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

// steve's line under the graphs: how many of its jobs are handed out and its settings, the one
// being changed highlighted; or that it does not run.
[[nodiscard]] std::vector<Span> steve_line(const std::optional<steve::Status>& steve,
                                           std::optional<std::size_t> editing, const Glyphs& glyph);

// The pressure panel from row down: what the history holds, with steve's limits marked.
[[nodiscard]] std::vector<std::vector<Span>> pressure_lines(const pressure::History& history,
                                                            const Limits& limits, std::size_t width,
                                                            const Glyphs& glyph);

// Moving and opening where there are tasks, and while steve's settings are being changed, how.
template <class S>
void draw_watch_hints(S& screen, const Watched& watched, const Glyphs& glyph, Size size) {
    using Hints = std::vector<std::pair<std::string_view, std::string_view>>;
    if (watched.editing) {
        draw_hints(screen, size.rows - 1, size.cols,
                   {{"h/l", "setting"}, {"+/-", "change"}, {"s", "done"}, {"q", "quit"}});
        return;
    }
    Hints hints{{"s", "steve"}, {"esc", "back"}, {"q", "quit"}};
    if (!watched.snapshots.empty()) {
        hints.insert(hints.begin(), {{glyph.move, "move"}, {glyph.enter, "open"}});
    }
    draw_hints(screen, size.rows - 1, size.cols, hints);
}

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
                {"emerge", tone_pen(Tone::name)}},
               glyph);
    const unsigned first = 2;
    // The pressure panel sits above the hints, a blank line above it, where there is room.
    const bool panel = size.rows >= first + pressure_rows + 7;
    const unsigned height = size.rows - first - 1 - (panel ? pressure_rows + 2 : 0);
    app.set_height(height);
    if (panel) {
        const auto graph = std::clamp<std::size_t>(size.cols > 60 ? size.cols - 60 : 0, 8, 120);
        unsigned row = size.rows - 2 - pressure_rows;
        for (const auto& spans :
             pressure_lines(watched.history, limits_of(watched.steve), graph, glyph)) {
            put_spans(screen, row++, 0, spans, size.cols);
        }
        put_spans(screen, row, 0, steve_line(watched.steve, watched.editing, glyph), size.cols);
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
        draw_watch_hints(screen, watched, glyph, size);
        return;
    }
    // Every line, with the selected one's index, then a window that keeps it in view.
    struct Line {
        std::vector<Span> spans;
        bool selected = false;
    };
    std::vector<Line> lines;
    const auto rows = watch_rows(watched);
    std::size_t selected_line = 0;
    for (std::size_t snapshot = 0; snapshot < watched.snapshots.size(); ++snapshot) {
        if (!lines.empty()) {
            lines.push_back({});
        }
        lines.push_back({.spans = emerge_spans(watched.snapshots.at(snapshot), glyph)});
        if (watched.plan_error && snapshot == rows_owner(watched)) {
            lines.push_back({.spans = {{std::format("   {} what the merge list waits for is "
                                                    "unknown: {}",
                                                    glyph.broken, *watched.plan_error),
                                        tone_pen(Tone::note)}}});
        }
        for (std::size_t at = 0; at < rows.size(); ++at) {
            const auto& row = rows.at(at);
            if (row.snapshot != snapshot) {
                continue;
            }
            const bool selected = at == watched.cursor.at;
            if (selected) {
                selected_line = lines.size();
            }
            std::vector<Span> spans{marker(selected, glyph), {"  ", {}}};
            std::string prefix;
            for (const bool rail : row.rails) {
                prefix += rail ? std::string{glyph.rail} : std::string{"  "};
            }
            prefix += std::format("{} ", row.last ? glyph.branch : glyph.tee);
            spans.push_back({std::move(prefix), tone_pen(Tone::note)});
            std::ranges::move(row_spans(row, watched.frame, glyph), std::back_inserter(spans));
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
    draw_watch_hints(screen, watched, glyph, size);
}

// A command's output: its tab-separated fields in columns, lined up across each run of rows with
// the same number of fields.
template <class S> void draw_output(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& output = *app.output();
    draw_title(screen, app, size.cols,
               {{std::format(" {} egraph ", glyph.package),
                 {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                {std::format(" :{}  ", output.command),
                 {.fg = palette::text, .bg = std::nullopt, .bold = true}},
                {std::format("{} lines", output.rows.size()), tone_pen(Tone::count)}},
               glyph);
    const unsigned first = 2;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    // Column widths for the run of rows each row belongs to.
    std::vector<std::vector<std::size_t>> widths(output.rows.size());
    for (std::size_t start = 0; start < output.rows.size();) {
        auto end = start;
        std::vector<std::size_t> run(output.rows.at(start).size(), 0);
        while (end < output.rows.size() && output.rows.at(end).size() == run.size()) {
            for (std::size_t column = 0; column < run.size(); ++column) {
                run.at(column) = std::max(run.at(column), columns(output.rows.at(end).at(column)));
            }
            ++end;
        }
        std::fill(widths.begin() + static_cast<std::ptrdiff_t>(start),
                  widths.begin() + static_cast<std::ptrdiff_t>(end), run);
        start = end;
    }
    for (unsigned line = 0; line < height; ++line) {
        const auto index = output.cursor.top + line;
        if (index >= output.rows.size()) {
            break;
        }
        const bool selected = index == output.cursor.at;
        const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
        if (selected) {
            screen.fill_row(first + line, {.fg = std::nullopt, .bg = palette::surface});
        }
        const auto& fields = output.rows.at(index);
        std::string text;
        for (std::size_t column = 0; column < fields.size(); ++column) {
            text += fields.at(column);
            if (column + 1 < fields.size()) {
                text +=
                    std::string(widths.at(index).at(column) - columns(fields.at(column)) + 2, ' ');
            }
        }
        const auto pen = output.links.at(index) ? Pen{} : tone_pen(Tone::note);
        put_spans(screen, first + line, 0, {marker(selected, glyph), {text, pen}}, size.cols, bg);
    }
    if (output.rows.empty()) {
        put_spans(screen, first, 5, {{"the command printed nothing", tone_pen(Tone::note)}},
                  size.cols);
    }
    draw_hints(screen, size.rows - 1, size.cols,
               {{glyph.move, "move"}, {glyph.enter, "open"}, {":", "command"}, {"esc", "back"}});
}

// The : prompt over the hint bar, or the command it runs.
template <class S> void draw_prompt(S& screen, const App& app, Size size) {
    const Pen bar{.fg = palette::text, .bg = palette::mantle};
    const auto row = size.rows - 1;
    screen.fill_row(row, bar);
    if (app.prompt()) {
        put_spans(screen, row, 0,
                  {{std::format(":{}", *app.prompt()), bar},
                   {" ", {.fg = std::nullopt, .bg = palette::mauve}}},
                  size.cols);
    } else if (app.command_requested()) {
        put_spans(screen, row, 0,
                  {{std::format(":{}", *app.command_requested()), bar},
                   {"  running", {.fg = palette::overlay, .bg = palette::mantle}}},
                  size.cols);
    }
}

template <class S> void draw(S& screen, App& app, const Glyphs& glyph) {
    const auto size = screen.size();
    screen.clear();
    if (size.rows >= 5 && size.cols >= 10) {
        if (!app.pages().empty()) {
            draw_page(screen, app, glyph, size);
        } else if (app.output()) {
            draw_output(screen, app, glyph, size);
        } else if (app.checked()) {
            draw_check(screen, app, glyph, size);
        } else if (app.watched()) {
            draw_watch(screen, app, glyph, size);
        } else if (app.planned()) {
            draw_plan(screen, app, glyph, size);
        } else {
            draw_list(screen, app, glyph, size);
        }
        if (app.prompt() || app.command_requested()) {
            draw_prompt(screen, app, size);
        }
        if (app.dialog()) {
            draw_dialog(screen, *app.dialog(), glyph, size);
        }
    }
    screen.render();
}

enum class Polled : std::uint8_t { idle, running, finished };

// Starts the job the app asks for, polls it, and hands its result to finish; drops it, stopping
// its work, once the app no longer asks.
template <class T, class Start, class Finish>
Polled poll(std::optional<Job<T>>& job, bool requested, const Start& start, const Finish& finish) {
    if (!requested) {
        job.reset();
        return Polled::idle;
    }
    if (!job) {
        job = start();
    }
    auto result = (*job)();
    if (!result) {
        return Polled::running;
    }
    job.reset();
    finish(std::move(*result));
    return Polled::finished;
}

// Runs until the user quits or input ends, making the fresh build a check asks for, or the
// rebuild, in the background once the waiting view is on screen; and looking at the stores'
// inputs every stale_interval, refreshing them in the background once they changed.
template <class S> void run(S& screen, App& app, const Glyphs& glyph, const Services& services) {
    const auto now = [&services] {
        return services.now ? services.now() : std::chrono::steady_clock::now();
    };
    const bool watches = services.stale && services.refresh && app.shared();
    auto next_check = now() + stale_interval;
    std::optional<Job<CheckResult>> check;
    std::optional<Job<RebuildResult>> rebuild;
    std::optional<Job<RefreshResult>> refresh;
    draw(screen, app, glyph);
    while (!app.done()) {
        const auto refreshed = poll(
            refresh, app.refresh_requested(), [&] { return services.refresh(); },
            [&](RefreshResult result) {
                app.finish_refresh(std::move(result));
                next_check = now() + (app.refresh_error() ? retry_interval : stale_interval);
            });
        bool found_stale = false;
        if (watches && !app.stale() && now() >= next_check) {
            next_check = now() + stale_interval;
            app.finish_stale_check(services.stale(*app.shared()));
            found_stale = app.stale();
        }
        const auto checked = poll(
            check, app.check_requested(),
            [&] {
                return services.check
                           ? services.check(app.installed())
                           : ready(CheckResult{std::unexpected(std::string{"no way to check"})});
            },
            [&](CheckResult result) { app.finish_check(std::move(result)); });
        const auto rebuilt = poll(
            rebuild, app.rebuild_requested(),
            [&] {
                return services.rebuild ? services.rebuild()
                                        : ready(RebuildResult{
                                              std::unexpected(std::string{"no way to rebuild"})});
            },
            [&](RebuildResult result) { app.finish_rebuild(std::move(result)); });
        if (checked == Polled::finished || rebuilt == Polled::finished ||
            refreshed == Polled::finished || found_stale) {
            // Drawn below before any key is read.
        } else if (const auto change = app.steve_change_requested()) {
            app.finish_steve_change(services.set_steve
                                        ? services.set_steve(change->setting, change->value)
                                        : std::unexpected(std::string{"no way to change steve"}));
        } else if (app.watch_requested()) {
            app.finish_watch(services.watch ? services.watch() : std::vector<emerge::Snapshot>{},
                             services.sample ? services.sample() : pressure::Sample{},
                             services.steve ? services.steve() : std::nullopt,
                             services.merge_list ? services.merge_list()
                                                 : std::vector<emerge::Pending>{});
        } else if (const auto& line = app.command_requested()) {
            app.finish_command(
                services.command
                    ? services.command(*line)
                    : Answer{.exit = Exit::failure, .out = {}, .err = "no way to run commands"});
        } else if (const auto list = app.plan_requested()) {
            app.finish_plan(services.plan ? services.plan(*list)
                                          : std::unexpected(std::string{"no way to plan"}));
        } else {
            auto timeout = app.refresh();
            if (watches && !app.stale()) {
                const auto due =
                    std::max(std::chrono::ceil<std::chrono::milliseconds>(next_check - now()),
                             std::chrono::milliseconds{0});
                timeout = timeout ? std::min(*timeout, due) : due;
            }
            app.handle(screen.read(timeout));
        }
        if (!app.done()) {
            draw(screen, app, glyph);
        }
    }
}

// Opens the terminal and runs the interface over the stores, first showing any warnings from
// opening them; errors go to err.
[[nodiscard]] Exit open_and_run(std::shared_ptr<const Stores> stores, bool dynamic_deps,
                                GlyphSet glyphs, const Services& services,
                                std::span<const std::string> warnings, std::ostream& err);

} // namespace egraph::tui
