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
#include "repository.hpp"
#include "screen.hpp"
#include "search.hpp"
#include "status.hpp"
#include "steve.hpp"
#include "store.hpp"
#include "use_stack.hpp"
#include "visibility.hpp"
#include "what_if.hpp"

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
// link.atom. A remedy row is a line of the commands past a held update. A version row is one of
// the package's versions in the repositories, or installed. A flag row is one of its version's
// USE flags, with where it was last set in text.
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
    remedy,
    version,
    flag
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
    std::optional<PackageVersion> version;
    std::optional<FlagState> flag;
    // For a flag row whose state the lines tried change, its state without them.
    std::optional<bool> was;
    // For a flag row, the width its flag is padded to.
    std::size_t column = 0;
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
using IndexResult = std::expected<std::shared_ptr<const RepositoryIndex>, std::string>;
// Loads the repository index, building it first where it is missing or stale.
using IndexLoader = std::function<Job<IndexResult>()>;
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

// The status file the watch service writes, and whether it is current against its stores.
struct StatusShown {
    Status status;
    bool current = false;
};
// Reads the status file, nothing when there is none.
using StatusReader = std::function<std::optional<StatusShown>()>;

// The notices watch wrote, as of now, less those the user set aside, and how many that was.
struct NoticesShown {
    std::vector<Notice> notices;
    std::size_t set_aside = 0;
};

using NoticesReader = std::function<std::optional<NoticesShown>()>;
// Dismisses a notice, or with a time puts it off that long.
using NoticeSetter = std::function<std::expected<void, std::string>(
    const Notice&, std::optional<std::chrono::seconds>)>;
// A news item's text, as lines.
using NewsText = std::function<std::expected<std::vector<std::string>, std::string>(const Notice&)>;
// Marks a news item read in portage's news files.
using NewsMarker = std::function<std::expected<void, std::string>(const Notice&)>;
// A program the interface steps aside for, given the terminal until it exits: its exit status.
using TerminalProgram = std::function<std::expected<int, std::string>()>;

// The pages of the package list, each the set its plan updates: -uDN @installed, @world or
// @system.
enum class Scope : std::uint8_t { installed, world, system };
inline constexpr std::array scopes{Scope::installed, Scope::world, Scope::system};
[[nodiscard]] std::string_view scope_name(Scope scope);

// What the interface carries out once confirmed: merges through egraph exec, removals through
// egraph remove, syncs through egraph sync.
struct Action {
    enum class Kind : std::uint8_t { install, update, rebuild, remove, sync };
    Kind kind = Kind::install;
    // Atoms, or for a sync repositories; an update without any takes every update of the scope's
    // set.
    std::vector<std::string> targets{};
    Scope scope = Scope::installed;
    // For a removal, as remove --with-bdeps.
    bool build_deps = true;
    bool operator==(const Action&) const = default;
};

// The command carrying the action out, without the program and --yes: install as exec, update as
// exec --oneshot -u -N (every update as -D and the scope's set), rebuild as exec --oneshot, remove
// as remove, sync as sync.
[[nodiscard]] std::vector<std::string> action_arguments(const Action& action);

// Whether the action's command shows what it would do before doing it: a sync does not.
[[nodiscard]] bool previewed(const Action& action);

// The action enter takes on a notice: a GLSA's packages updated, a package missing libraries
// rebuilt, @preserved-rebuild, a stale repository synced; none for the rest.
[[nodiscard]] std::optional<Action> notice_action(const Notice& notice);

// What enter does on a notice, for the key bar: "update", "rebuild", "sync", "open", "read",
// "dispatch-conf"; empty where it does nothing.
[[nodiscard]] std::string_view notice_work(const Notice& notice);

// What the action would do, as its command shows it before asking, in the human layout.
struct Preview {
    // There is something to do, and the system can be changed: confirming runs it.
    bool ready = false;
    std::string out{};
    std::string err{};
};
using Previewer = std::function<Preview(const Action&)>;

// A package that failed in a run: portage's log of it and the log's last lines, or no log for a
// worker that stopped without one.
struct RunFailure {
    std::string cpv{};
    std::string log{};
    std::vector<std::string> tail{};
};
// How a run ended: why it could not start or be waited for, else its exit status, and after a
// failure what failed, then where the run's output went and its last lines.
struct RunResult {
    std::string error{};
    int status = 0;
    std::vector<RunFailure> failures{};
    std::string output{};
    std::vector<std::string> tail{};
};
// Starts a confirmed action beside the interface; the job ends with the run. Dropped, as when the
// interface quits, it leaves the run going.
using Runner = std::function<Job<RunResult>(const Action&)>;

// The last count lines of text as a terminal would leave them: escape sequences and other
// control characters dropped, each line what its last carriage return left.
[[nodiscard]] std::vector<std::string> plain_tail(std::string_view text, std::size_t count);

// Lines to try on stores, untried.
struct TryRequest {
    std::shared_ptr<const Stores> stores;
    std::vector<WhatIfLine> lines;
};
struct Tried {
    // The stores the lines were tried on: those asked for, or new ones where the cps the lines
    // newly reach were evaluated first.
    std::shared_ptr<const Stores> untried;
    std::shared_ptr<const Stores> tried;
    // Cps the lines newly reach, not evaluated (--no-refresh).
    std::vector<std::string> unevaluated{};
};
using TryResult = std::expected<Tried, std::string>;
// Tries lines as with_what_if does, evaluating the cps they newly reach.
using Trier = std::function<Job<TryResult>(const TryRequest&)>;

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
    // Empty where there is no repository index to search.
    IndexLoader load_index{};
    Previewer preview{};
    Runner run{};
    // The steady clock when empty.
    Clock now{};
    StatusReader status{};
    NoticesReader notices{};
    NoticeSetter set_aside{};
    NewsText news_text{};
    NewsMarker mark_read{};
    TerminalProgram dispatch_conf{};
    Trier try_lines{};
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
// its packages (the only one, if there is one), never to egraph exec, which keeps none there;
// its tasks not on the list come first.
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
    // A tick makes it due, read once watch_interval has passed since the last read; what shows
    // it out of date makes it urgent, read at once.
    bool due = true;
    bool urgent = true;
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

// A message over whatever is on screen, which the next key dismisses; a question waits for y or
// n. The move keys scroll one taller than the screen.
struct Dialog {
    bool error = true;
    std::string title;
    std::vector<std::string> lines;
    bool question = false;
    // The first line shown.
    std::size_t top = 0;
    // The keys that close it, in place of " any key " or " y yes  n no ".
    std::string closing{};
};

// A notice to dismiss, or to put off for a while, waiting for run() to do it.
struct NoticeChange {
    Notice notice;
    std::optional<std::chrono::seconds> later{};
    // Marked read in portage's news files, rather than set aside.
    bool read = false;
};

// The choices for putting a notice off, by the digit choosing each.
inline constexpr std::array put_off_choices{
    std::pair{std::string_view{"an hour"}, std::chrono::seconds{std::chrono::hours{1}}},
    std::pair{std::string_view{"a day"}, std::chrono::seconds{std::chrono::days{1}}},
    std::pair{std::string_view{"a week"}, std::chrono::seconds{std::chrono::weeks{1}}}};

// A line of the plan view: a root set at depth 0, then the packages down the chain from it to
// each merge, as updates --tree draws them.
struct PlanRow {
    // The set ("@selected", empty for merges nothing keeps), or a package's cpv.
    std::string label;
    // Index into Plan::merges, for a merge; the packages between merges are installed ones.
    std::optional<std::uint32_t> merge;
    // Index into ScopePlan::tried, for a merge the lines tried drop, and the heading over them.
    std::optional<std::size_t> dropped{};
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

// A page's plan, and what the list shows of it per package.
// A merge the lines tried change, as tried_lines records it: by its first field (the installed
// cpv it replaces, or a new package's), added, changed, or dropped (with the fields it had).
struct TriedMerge {
    enum class Change : std::uint8_t { added, changed, dropped };
    std::string first;
    Change change = Change::added;
    std::string kind;
    std::string target;
    std::string repo;
    std::string flags;
};

// tried_lines' records.
[[nodiscard]] std::vector<TriedMerge> tried_merges(std::span<const std::string> records);

struct ScopePlan {
    Scope scope = Scope::installed;
    Plan plan;
    std::vector<std::optional<PendingUpdate>> updates;
    // What the merge a slot-operator rebuild is for; empty otherwise.
    std::vector<std::string> rebuilt_for;
    // The held update, as an index into plan.held and remedies.
    std::vector<std::optional<std::uint32_t>> held;
    std::vector<Remedy> remedies;
    // Why the set could not be planned.
    std::optional<std::string> error;
    // The stores it was made for, as App counts them.
    std::size_t generation = 0;
    // With lines tried, what they change against the plan made without them.
    std::vector<TriedMerge> tried{};
};

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
    // The search through the repositories: the key as typed, and what the last one run found.
    struct Search {
        std::string query;
        bool typing = true;
        // As emerge --searchdesc.
        bool descriptions = false;
        // The key the results are for, once one ran; one waiting for the index, before.
        std::optional<std::string> ran;
        bool pending = false;
        std::vector<Found> results;
        Cursor cursor;
    };
    // A package of the repositories that is not installed: what its best version's ebuild says,
    // and every version.
    struct Listing {
        Found found;
        std::vector<Row> rows;
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
    // Open from the list, under any pages opened from it.
    [[nodiscard]] const std::optional<Search>& search() const { return search_; }
    // Opened from the search, under any pages opened from it.
    [[nodiscard]] const std::optional<Listing>& listing() const { return listing_; }
    // The repository index's packages, once loaded.
    [[nodiscard]] const std::optional<Catalogue>& catalogue() const { return catalogue_; }
    // Whether the search waits for the repository index, which run() then loads.
    [[nodiscard]] bool index_requested() const { return search_ && !indexed_ && !index_failed_; }
    void finish_index(IndexResult result);
    // What emerge -uDN would merge for the package, as the page's plan weighs it.
    [[nodiscard]] const std::optional<PendingUpdate>& update_of(std::uint32_t package) const {
        return current().updates.at(package);
    }
    // The package's update the plan holds back, as an index into plan().held and remedies().
    [[nodiscard]] std::optional<std::uint32_t> held_of(std::uint32_t package) const {
        return current().held.at(package);
    }
    [[nodiscard]] const Plan& plan() const { return current().plan; }
    // What the lines tried change in the page's plan, by the merge's first field.
    [[nodiscard]] std::optional<TriedMerge> tried_of(std::string_view first) const;
    [[nodiscard]] const std::vector<TriedMerge>& current_tried() const { return current().tried; }
    [[nodiscard]] const std::vector<Remedy>& remedies() const { return current().remedies; }
    // The list's page, whose set the plan updates.
    [[nodiscard]] Scope scope() const { return scope_; }
    // Whether the scope has its plan made, for these stores.
    [[nodiscard]] bool scope_planned(Scope scope) const;
    // Why the page's set could not be planned.
    [[nodiscard]] const std::optional<std::string>& scope_error() const { return current().error; }
    // Whether run() is to make a page's plan in the background: the page's, missing, or one
    // being made.
    [[nodiscard]] bool scope_plan_requested() const;
    // The scope whose plan is being made.
    [[nodiscard]] std::optional<Scope> planning() const { return planning_; }
    // Makes the page's missing plan beside the caller.
    [[nodiscard]] Job<ScopePlan> start_scope_plan();
    void finish_scope_plan(ScopePlan plan);
    // The status file the watch service writes, as last read.
    [[nodiscard]] const std::optional<StatusShown>& status() const { return status_; }
    void finish_status(std::optional<StatusShown> status) { status_ = std::move(status); }
    [[nodiscard]] const std::optional<NoticesShown>& notices() const { return notices_; }
    // Keeps the cursor on the notice it was on, where that is still shown.
    void finish_notices(std::optional<NoticesShown> notices);
    // The notices page, after the sets, in place of the list.
    [[nodiscard]] bool on_notices() const { return on_notices_; }
    // Turns to the notices page, at once or once there are notices.
    void open_notices();
    [[nodiscard]] const Cursor& notice_cursor() const { return notice_cursor_; }
    // Choosing how long to put the selected notice off for.
    [[nodiscard]] bool putting_off() const { return putting_off_; }
    [[nodiscard]] const std::optional<NoticeChange>& notice_change_requested() const {
        return notice_change_;
    }
    // Drops the notice from the page once set aside or read; shows why it could not be, and sets
    // aside a news item that could not be marked read.
    void finish_notice_change(const std::expected<void, std::string>& result);
    // The news item whose text enter asked for.
    [[nodiscard]] const std::optional<Notice>& news_requested() const { return news_wanted_; }
    // Shows the text, marking the item read once closed.
    void finish_news(const std::expected<std::vector<std::string>, std::string>& text);
    // Enter on the configuration notice: dispatch-conf, the interface stepping aside for it.
    [[nodiscard]] bool dispatch_conf_requested() const { return dispatching_; }
    // Says why dispatch-conf could not run or failed; the notices are read again after it.
    void finish_dispatch_conf(const std::expected<int, std::string>& ran);
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
    // The stores shown, shared, without the lines tried; empty for an app over an installed
    // store alone.
    [[nodiscard]] std::shared_ptr<const Stores> shared() const;
    // What-if lines toggled, in the order first toggled; the stores shown are tried with them
    // once run() has made them.
    [[nodiscard]] const std::vector<WhatIfLine>& what_if() const { return lines_; }
    // The evaluated store without the lines: the one shown while none is tried.
    [[nodiscard]] const Evaluated& untried() const {
        return untried_ ? untried_->evaluated : evaluated();
    }
    // Whether run() is to try the lines in the background: missing, or a try being made.
    [[nodiscard]] bool try_requested() const {
        return owned_ && !lines_.empty() && (trying_ || tried_version_ != version_);
    }
    [[nodiscard]] bool trying() const { return trying_; }
    [[nodiscard]] TryRequest start_try();
    // Shows the stores tried, each view where it was, unless the lines changed meanwhile.
    void finish_try(TryResult result);
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
    // The installed packages picked in the list, by cpv, for an update or a removal.
    [[nodiscard]] const std::vector<std::string>& picked() const { return picked_; }
    // An action waiting for run() to preview it, for a dialog to confirm.
    [[nodiscard]] const std::optional<Action>& preview_requested() const { return previewing_; }
    void finish_preview(const Preview& preview);
    // The confirmed action, which run() starts and polls to its end.
    [[nodiscard]] const std::optional<Action>& run_requested() const { return running_; }
    void finish_run(RunResult result);
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
    // Whether ? has the view's keys listed over it, until the next key.
    [[nodiscard]] bool keys_shown() const { return keys_shown_; }
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
    // Lines a dialog has room for; drawing sets it, and scrolling uses it.
    void set_dialog_height(std::size_t rows) { dialog_height_ = std::max<std::size_t>(rows, 1); }

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
    void own(std::shared_ptr<const Loaded> loaded);
    // Stores to try the lines on from now on, if there are lines.
    void rebase(const std::shared_ptr<const Stores>& stores);
    // Toggles the flag for atom: takes back the line's token for it, or adds one turning it.
    void toggle(const FlagState& flag, const std::string& atom);
    // The page's plan, or none yet.
    [[nodiscard]] const ScopePlan& current() const;
    // Moves the list to the page step pages on, the first and last staying put.
    void turn_page(int step);
    void filter();
    void recompute();
    void open(std::uint32_t package);
    // The search's results for its key, or nothing until the index is loaded.
    void run_search();
    void open_listing(const Found& found);
    // A cp's versions as rows, under a heading, where the index has any.
    [[nodiscard]] std::vector<Row> version_rows(std::string_view cp) const;
    void handle_search(const Key& key);
    void handle_listing(const Key& key);
    // Whether the view on top takes keys as text: the filter or the search being typed.
    [[nodiscard]] bool typing() const;
    void unfold(Page& page);
    void fold(Page& page);
    void handle_list(const Key& key);
    void handle_notices(const Key& key);
    void work_on(const Notice& notice);
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
    // Has run() preview action, unless a run is going.
    void act(Action action);
    // Installs the version, which a repository must hold.
    void install(std::string_view cp, const PackageVersion& version);
    void handle_dialog(const Key& key);

    std::reference_wrapper<const Store> store_;
    std::reference_wrapper<const Store> installed_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::reference_wrapper<const Graph> graph_;
    // Behind a pointer so the references stay valid when the app moves; shared with a plan
    // made in the background.
    std::shared_ptr<const Loaded> owned_;
    // The repository index and its masks, behind a pointer as the stores are.
    struct Indexed {
        explicit Indexed(std::shared_ptr<const RepositoryIndex> loaded)
            : index{std::move(loaded)}, masks{*index} {}
        std::shared_ptr<const RepositoryIndex> index;
        VersionMasks masks;
    };
    std::unique_ptr<const Indexed> indexed_;
    // Over indexed_ and the installed store shown.
    std::optional<Catalogue> catalogue_;
    bool index_failed_ = false;
    std::optional<Search> search_;
    std::optional<Listing> listing_;
    bool dynamic_deps_ = false;
    Update update_;
    Source source_ = Source::opened;
    std::vector<std::string> folded_;
    std::vector<std::size_t> dependencies_;
    std::vector<std::size_t> dependents_;
    std::vector<std::size_t> broken_;
    std::vector<std::size_t> broken_at_run_time_;
    std::vector<Masking> masking_;
    // By scope.
    std::array<std::optional<ScopePlan>, scopes.size()> plans_;
    // A plan of nothing, shown while the page's is made.
    ScopePlan blank_;
    Scope scope_ = Scope::installed;
    std::optional<Scope> planning_;
    // Counts the stores shown, so that a plan made for others is dropped.
    std::size_t generation_ = 0;
    std::vector<WhatIfLine> lines_;
    // The stores shown without the lines, while a try is shown.
    std::shared_ptr<const Stores> untried_;
    // Counts changes to the lines and to the stores they are tried on; tried_version_ is the
    // count the stores shown were tried at, try_version_ that of the try being made.
    std::uint64_t version_ = 0;
    std::uint64_t tried_version_ = 0;
    std::uint64_t try_version_ = 0;
    bool trying_ = false;
    std::optional<StatusShown> status_;
    std::optional<NoticesShown> notices_;
    bool on_notices_ = false;
    bool notices_first_ = false;
    Cursor notice_cursor_;
    bool putting_off_ = false;
    std::optional<NoticeChange> notice_change_;
    std::optional<Notice> news_wanted_;
    bool dispatching_ = false;
    // The news item the dialog shows.
    std::optional<Notice> reading_;
    bool build_deps_ = true;
    Kept kept_;
    std::vector<std::optional<std::uint32_t>> root_of_;
    List list_;
    std::optional<Checked> checked_;
    std::optional<Watched> watched_;
    std::optional<Planned> planned_;
    std::vector<Page> pages_;
    std::optional<Dialog> dialog_;
    bool keys_shown_ = false;
    std::vector<std::string> picked_;
    std::optional<Action> previewing_;
    // The action the dialog asks to run; while a run goes, an empty one asks to quit.
    std::optional<Action> confirming_;
    std::optional<Action> running_;
    std::size_t dialog_height_ = 1;
    std::optional<std::string> prompt_;
    std::optional<std::string> command_;
    std::optional<Output> output_;
    std::optional<std::string> stale_;
    std::optional<std::string> refresh_error_;
    std::size_t frame_ = 0;
    std::size_t height_ = 1;
    bool done_ = false;
};

// A key and what it does: the hint bar shows those for what is selected, and ? lists them all.
struct Hint {
    std::string key;
    std::string meaning;
    bool bar = false;
    auto operator<=>(const Hint&) const = default;
};

// The keys of the view on top.
[[nodiscard]] std::vector<Hint> view_keys(const App& app, const Glyphs& glyph);

// The ? overlay: every key of the view on top, aligned.
[[nodiscard]] Dialog keys_dialog(std::span<const Hint> hints);
// How long to put a notice off for.
[[nodiscard]] Dialog put_off_dialog();

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

// The hint bar: the keys for what is selected, and ? on the right where the view has more.
template <class S>
void draw_hints(S& screen, unsigned row, unsigned width, std::span<const Hint> hints) {
    const Pen bar{.fg = palette::overlay, .bg = palette::mantle};
    const Pen key{.fg = palette::mauve, .bg = palette::mantle, .bold = true};
    screen.fill_row(row, bar);
    std::vector<Span> spans{{" ", bar}};
    for (const auto& hint : hints) {
        if (hint.bar) {
            spans.push_back({hint.key, key});
            spans.push_back({std::format(" {}  ", hint.meaning), bar});
        }
    }
    put_spans(screen, row, 0, spans, width);
    if (std::ranges::any_of(hints, [](const Hint& hint) { return !hint.bar; }) && width > 8) {
        put_spans(screen, row, width - 8, {{"?", key}, {" keys  ", bar}}, width);
    }
}

// The trail, and on the right a refresh under way or failed, and a warning while a fresh build
// is shown without being saved.
template <class S>
void draw_title(S& screen, const App& app, unsigned width, const std::vector<Span>& trail,
                const Glyphs& glyph) {
    screen.fill_row(0, {.fg = palette::text, .bg = palette::crust});
    put_spans(screen, 0, 0, trail, width, palette::crust);
    std::vector<Span> badges;
    if (const auto& action = app.run_requested()) {
        badges.push_back({std::format(" {} egraph {} ", spinner_frame(app.frame(), glyph),
                                      action_arguments(*action).front()),
                          {.fg = palette::crust, .bg = palette::mauve, .bold = true}});
    }
    if (app.trying()) {
        badges.push_back({.text = std::format(" {} trying ", spinner_frame(app.frame(), glyph)),
                          .pen = {.fg = palette::overlay, .bg = palette::crust}});
    }
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
    const auto tried = app.tried_of(app.store().string(app.store().packages.at(package).cpv));
    if (tried && tried->change == TriedMerge::Change::dropped) {
        spans = {{.text = "  ", .pen = {}},
                 {.text = tried->kind == "rebuild" ? std::string{"rebuild"}
                                                   : std::format("{} {}", glyph.instead,
                                                                 split_cpv(tried->target).version),
                  .pen = tone_pen(Tone::note)},
                 {.text = "  dropped", .pen = tone_pen(Tone::note)}};
    }
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
        if (tried) {
            spans.push_back(
                {.text = tried->change == TriedMerge::Change::added ? "  + tried" : "  ~ tried",
                 .pen = tone_pen(Tone::count)});
        }
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

// The pages as tabs, the list's picked out, one being planned spinning.
inline std::vector<Span> page_spans(const App& app, const Glyphs& glyph) {
    const Pen picked{.fg = palette::crust, .bg = palette::mauve, .bold = true};
    std::vector<Span> spans{{" ", {}}};
    if (!app.has_evaluated()) {
        spans.push_back({" packages ", app.on_notices() ? tone_pen(Tone::note) : picked});
    }
    for (const auto scope : app.has_evaluated() ? std::span{scopes} : std::span<const Scope>{}) {
        const bool shown = !app.on_notices() && scope == app.scope();
        const bool waiting = app.planning() == scope || (shown && !app.scope_planned(scope));
        auto text =
            waiting ? std::format(" {} {} ", spinner_frame(app.frame(), glyph), scope_name(scope))
                    : std::format(" {} ", scope_name(scope));
        spans.push_back({std::move(text), shown ? picked : tone_pen(Tone::note)});
    }
    if (const auto& notices = app.notices()) {
        const auto count = notices->notices.size();
        spans.push_back({std::format(" notices {} ", count),
                         app.on_notices() ? picked : tone_pen(count ? Tone::bad : Tone::note)});
    }
    return spans;
}

// The status file's plan of @world for the corner: its counts by glyph, whether emerge would
// refuse it, the repository synced longest ago, and whether the stores changed since.
[[nodiscard]] std::vector<Span> status_spans(const StatusShown& shown, const Glyphs& glyph,
                                             Seconds now);

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
    if (!app.picked().empty()) {
        title.push_back({std::format("  {} picked", app.picked().size()), tone_pen(Tone::choice)});
    }
    if (list.only == Only::orphans && (store.roots.empty() || !app.kept().unresolved.empty())) {
        title.push_back(
            {std::format("  {} depclean would refuse to run", glyph.broken), tone_pen(Tone::bad)});
    }
    draw_title(screen, app, size.cols, title, glyph);

    std::vector<Span> search;
    if (app.has_evaluated() || app.notices()) {
        search = page_spans(app, glyph);
        search.push_back({" ", {}});
    }
    search.push_back({std::format(" {} ", glyph.search), tone_pen(Tone::heading)});
    if (list.searching || !list.query.empty()) {
        search.push_back({list.query, {.fg = palette::text, .bg = std::nullopt, .bold = true}});
        if (list.searching) {
            search.push_back({" ", {.fg = std::nullopt, .bg = palette::mauve}});
        }
    } else {
        search.push_back({"/ to search", tone_pen(Tone::note)});
    }
    put_spans(screen, 1, 0, search, size.cols);
    if (const auto& shown = app.status(); shown && app.has_evaluated()) {
        const auto corner = status_spans(
            *shown, glyph,
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        std::size_t used = 0;
        for (const auto& span : search) {
            used += columns(span.text);
        }
        std::size_t wanted = 0;
        for (const auto& span : corner) {
            wanted += columns(span.text);
        }
        if (used + wanted + 2 <= size.cols) {
            put_spans(screen, 1, static_cast<unsigned>(size.cols - wanted), corner, size.cols);
        }
    }

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
        auto mark = marker(selected, glyph);
        if (std::ranges::contains(app.picked(), store.string(store.packages.at(id).cpv))) {
            mark.text = std::format(" {}{}", selected ? glyph.cursor : " ", glyph.good);
        }
        std::vector<Span> spans{
            mark, keep_mark(app, id, glyph), broken_mark(app, id, glyph), {" ", {}}};
        std::ranges::move(cpv_spans(store.string(store.packages.at(id).cpv)),
                          std::back_inserter(spans));
        std::ranges::move(update_mark(app, id, glyph), std::back_inserter(spans));
        put_spans(screen, first + line, 0, spans, right, bg);
        put_spans(screen, first + line, right,
                  {{std::format("{:>7}", app.dependencies(id)), tone_pen(Tone::version)},
                   {std::format("{:>11}", app.dependents(id)), tone_pen(Tone::count)}},
                  size.cols, bg);
    }
    if (const auto& error = app.scope_error()) {
        put_spans(screen, first, 5,
                  {{std::format("{} {}", glyph.broken, *error), tone_pen(Tone::bad)}}, size.cols);
    } else if (app.has_evaluated() && !app.scope_planned(app.scope()) &&
               list.only == Only::updates) {
        put_spans(screen, first, 5,
                  {{std::format("{} planning {}", spinner_frame(app.frame(), glyph),
                                scope_name(app.scope())),
                    tone_pen(Tone::note)}},
                  size.cols);
    } else if (list.shown.empty()) {
        put_spans(screen, first, 5, {{std::string{empty_list(list)}, tone_pen(Tone::note)}},
                  size.cols);
    }
}

inline Tone notice_tone(NoticeKind kind) {
    switch (kind) {
    case NoticeKind::glsa:
    case NoticeKind::masked:
    case NoticeKind::missing:
        return Tone::bad;
    case NoticeKind::preserved:
        return Tone::use;
    case NoticeKind::config:
    case NoticeKind::plan:
    case NoticeKind::check:
        return Tone::choice;
    case NoticeKind::news:
        return Tone::heading;
    case NoticeKind::stale:
        return Tone::note;
    }
    return Tone::note;
}

// The notices, one a row with its kind and age, and the selected one's detail below them.
template <class S> void draw_notices(S& screen, App& app, const Glyphs& glyph, Size size) {
    static const std::vector<Notice> none;
    const auto& shown = app.notices();
    const auto& notices = shown ? shown->notices : none;
    std::vector<Span> title{
        {std::format(" {} egraph ", glyph.package),
         {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
        {std::format(" {}  ", app.store().meta.eroot), tone_pen(Tone::note)},
        {notices.size() == 1 ? std::string{"1 notice"} : std::format("{} notices", notices.size()),
         tone_pen(Tone::count)}};
    if (shown && shown->set_aside != 0) {
        title.push_back({std::format("  {} set aside", shown->set_aside), tone_pen(Tone::note)});
    }
    draw_title(screen, app, size.cols, title, glyph);
    put_spans(screen, 1, 0, page_spans(app, glyph), size.cols);

    const auto& cursor = app.notice_cursor();
    const unsigned first = 3;
    const unsigned room = size.rows > first + 1 ? size.rows - first - 1 : 0;
    std::span<const std::string> detail;
    if (cursor.at < notices.size()) {
        detail = notices.at(cursor.at).detail;
    }
    const auto detail_rows = static_cast<unsigned>(std::min<std::size_t>(detail.size(), room / 2));
    const unsigned height = detail_rows ? room - detail_rows - 1 : room;
    app.set_height(height);
    put_spans(screen, 2, 0, {{"      kind      notice", tone_pen(Tone::note)}}, size.cols);
    if (notices.empty()) {
        put_spans(screen, first, 5, {{"Nothing needs you", tone_pen(Tone::note)}}, size.cols);
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    for (unsigned line = 0; line < height; ++line) {
        const auto index = cursor.top + line;
        if (index >= notices.size()) {
            break;
        }
        const auto& notice = notices.at(index);
        const bool selected = index == cursor.at;
        const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
        if (selected) {
            screen.fill_row(first + line, {.fg = std::nullopt, .bg = palette::surface});
        }
        const auto age = age_text(notice.since, now);
        const auto right =
            size.cols > columns(age) + 2 ? static_cast<unsigned>(size.cols - columns(age) - 2) : 0;
        put_spans(screen, first + line, 0,
                  {marker(selected, glyph),
                   {std::format(" {:<10}", notice_kind_name(notice.kind)),
                    tone_pen(notice_tone(notice.kind))},
                   {notice.title, {}}},
                  right, bg);
        put_spans(screen, first + line, right, {{age, tone_pen(Tone::note)}}, size.cols, bg);
    }
    for (unsigned line = 0; line < detail_rows; ++line) {
        put_spans(screen, first + height + 1 + line, 6, {{detail[line], {}}}, size.cols);
    }
}

// A version of a package: whether it is installed, the version (in the bad tone when masked),
// slot, repository, and why it is masked.
[[nodiscard]] std::vector<Span> version_spans(const PackageVersion& version, const Glyphs& glyph);

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
        case RowType::flag:
            if (row.flag) {
                const auto& flag = *row.flag;
                const bool selected = index == page.cursor.at;
                const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
                if (selected) {
                    screen.fill_row(at, {.fg = std::nullopt, .bg = palette::surface});
                }
                const auto spelled =
                    std::format("{}{}{}{}", flag.fixed ? "(" : "", flag.enabled ? "" : "-",
                                flag.flag, flag.fixed ? ")" : "");
                put_spans(
                    screen, at, 0,
                    {marker(selected, glyph),
                     {spelled, tone_pen(flag.enabled ? Tone::use : Tone::note)},
                     {std::string(row.column > spelled.size() ? row.column - spelled.size() : 2,
                                  ' '),
                      {}},
                     {row.text, tone_pen(Tone::note)},
                     {row.was ? (*row.was ? "  was on" : "  was off") : "", tone_pen(Tone::count)}},
                    size.cols, bg);
            }
            break;
        case RowType::version:
            if (row.version) {
                const bool selected = index == page.cursor.at;
                const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
                if (selected) {
                    screen.fill_row(at, {.fg = std::nullopt, .bg = palette::surface});
                }
                std::vector<Span> spans{marker(selected, glyph)};
                std::ranges::move(version_spans(*row.version, glyph), std::back_inserter(spans));
                put_spans(screen, at, 0, spans, size.cols, bg);
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
    std::vector<std::size_t> earlier;
    for (const auto& wait : merge.waits) {
        const auto place = planned.places.at(wait.merge);
        if (ordering(wait.kinds) && place < planned.places.at(index)) {
            earlier.push_back(place);
        }
    }
    if (!earlier.empty()) {
        std::ranges::sort(earlier);
        std::string waits;
        for (const auto place : earlier) {
            waits += std::format("{}{}", waits.empty() ? "" : " ", place);
        }
        spans.push_back({std::format("  {} ", glyph.waiting), tone_pen(Tone::note)});
        spans.push_back({std::move(waits), tone_pen(Tone::count)});
    }
    if (const auto beside = other_slots(app.store(), evaluated, merge); !beside.empty()) {
        spans.push_back({"  beside", tone_pen(Tone::note)});
        for (const auto id : beside) {
            const auto& pkg = app.store().packages.at(id);
            spans.push_back({std::format(" {}", split_cpv(app.store().string(pkg.cpv)).version),
                             tone_pen(Tone::version)});
            spans.push_back(
                {std::format(":{}", app.store().string(pkg.slot)), tone_pen(Tone::note)});
        }
    }
    return spans;
}

// What the lines tried change in the page's plan, counted as updates counts them.
inline std::string tried_count(const App& app) {
    const auto& tried = app.current_tried();
    if (tried.empty()) {
        return "";
    }
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t dropped = 0;
    for (const auto& merge : tried) {
        (merge.change == TriedMerge::Change::added     ? added
         : merge.change == TriedMerge::Change::changed ? changed
                                                       : dropped)++;
    }
    std::string text;
    for (const auto& [count, words] :
         {std::pair{added, "more"}, {dropped, "fewer"}, {changed, "changed"}}) {
        if (count != 0) {
            text += std::format("{}{} {}", text.empty() ? "  tried: " : ", ", count, words);
        }
    }
    return text;
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
                {std::format("  {} merges", app.plan().merges.size()), tone_pen(Tone::count)},
                {tried_count(app), tone_pen(Tone::note)}},
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
        if (row.depth == 0 && row.dropped) {
            put_spans(screen, at, 3, {{"dropped by what is tried", tone_pen(Tone::note)}},
                      size.cols);
            continue;
        }
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
        if (row.dropped) {
            // As updates lists it: the merge's kind, name, version and any it moved to.
            const auto& tried = app.current_tried().at(*row.dropped);
            const auto from = split_cpv(tried.first);
            const auto mark = tried.kind == "upgrade"     ? glyph.upgrade
                              : tried.kind == "downgrade" ? glyph.downgrade
                              : tried.kind == "rebuild"   ? glyph.rebuild
                                                          : glyph.added;
            auto text = std::format("{} {}/{}  {}", mark, from.category, from.name, from.version);
            if (tried.first != tried.target) {
                text += std::format(" {} {}", glyph.instead, split_cpv(tried.target).version);
            }
            spans.push_back({.text = "- ", .pen = tone_pen(Tone::bad)});
            spans.push_back({.text = std::format("{}  ::{}  (dropped)", text, tried.repo),
                             .pen = tone_pen(Tone::note)});
            put_spans(screen, at, 0, spans, size.cols, bg);
            continue;
        }
        if (row.merge && !app.current_tried().empty()) {
            const auto& merge = app.plan().merges.at(*row.merge);
            const auto first_field =
                merge.replaces
                    ? app.store().string(app.store().packages.at(*merge.replaces).cpv)
                    : app.evaluated().string(app.evaluated().candidates.at(merge.candidate).cpv);
            const auto tried = app.tried_of(first_field);
            spans.push_back({.text = !tried                                       ? "  "
                                     : tried->change == TriedMerge::Change::added ? "+ "
                                                                                  : "~ ",
                             .pen = tone_pen(Tone::count)});
        }
        std::ranges::move(row.merge ? merge_spans(app, planned, *row.merge, glyph)
                                    : cpv_spans(row.label),
                          std::back_inserter(spans));
        put_spans(screen, at, 0, spans, size.cols, bg);
    }
    if (planned.rows.empty()) {
        put_spans(screen, first, 5, {{"nothing to merge", tone_pen(Tone::note)}}, size.cols);
    }
}

// A search result: whether it is installed, its cp, best version (in the bad tone when masked),
// the latest installed, and its description.
[[nodiscard]] std::vector<Span> found_spans(const Found& found, const Glyphs& glyph);

template <class S> void draw_search(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& open = app.search();
    if (!open) {
        return;
    }
    const auto& search = *open;
    std::vector<Span> title{{std::format(" {} egraph ", glyph.package),
                             {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                            {std::format(" {} ", glyph.trail), tone_pen(Tone::note)},
                            {"search", tone_pen(Tone::name)}};
    if (search.ran) {
        title.push_back({std::format("  {} found", search.results.size()), tone_pen(Tone::count)});
    }
    if (search.descriptions) {
        title.push_back({"  descriptions too", tone_pen(Tone::note)});
    }
    draw_title(screen, app, size.cols, title, glyph);

    std::vector<Span> box{{std::format(" {} ", glyph.search), tone_pen(Tone::heading)}};
    if (search.typing || !search.query.empty()) {
        box.push_back({search.query, {.fg = palette::text, .bg = std::nullopt, .bold = true}});
        if (search.typing) {
            box.push_back({" ", {.fg = std::nullopt, .bg = palette::mauve}});
        }
    } else {
        box.push_back({"/ to search", tone_pen(Tone::note)});
    }
    put_spans(screen, 1, 0, box, size.cols);

    const unsigned first = 3;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    if (app.index_requested()) {
        put_spans(screen, first, 3,
                  {{spinner_frame(app.frame(), glyph), tone_pen(Tone::heading)},
                   {" Loading the repository index; building it the first time takes a while",
                    tone_pen(Tone::note)}},
                  size.cols);
    } else if (search.ran && search.results.empty()) {
        put_spans(screen, first, 5, {{"no package matches", tone_pen(Tone::note)}}, size.cols);
    } else if (search.ran) {
        put_spans(
            screen, 2, 0,
            {{std::format("{:<47}{:<15}{:<15}description", "     package", "version", "installed"),
              tone_pen(Tone::note)}},
            size.cols);
    }
    for (unsigned line = 0; line < height && !app.index_requested(); ++line) {
        const auto index = search.cursor.top + line;
        if (index >= search.results.size()) {
            break;
        }
        const bool selected = index == search.cursor.at;
        const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
        if (selected) {
            screen.fill_row(first + line, {.fg = std::nullopt, .bg = palette::surface});
        }
        std::vector<Span> spans{marker(selected, glyph)};
        std::ranges::move(found_spans(search.results.at(index), glyph), std::back_inserter(spans));
        put_spans(screen, first + line, 0, spans, size.cols, bg);
    }
}

template <class S> void draw_listing(S& screen, App& app, const Glyphs& glyph, Size size) {
    const auto& open = app.listing();
    if (!open) {
        return;
    }
    const auto& listing = *open;
    const auto cp = split_cpv(listing.found.cp);
    draw_title(screen, app, size.cols,
               {{std::format(" {} egraph ", glyph.package),
                 {.fg = palette::mauve, .bg = std::nullopt, .bold = true}},
                {std::format(" {} ", glyph.trail), tone_pen(Tone::note)},
                {"search", tone_pen(Tone::category)},
                {std::format(" {} ", glyph.trail), tone_pen(Tone::note)},
                {std::string{cp.name}, tone_pen(Tone::name)}},
               glyph);
    auto heading = cpv_spans(listing.found.cp);
    heading.insert(heading.begin(), {std::format(" {} ", glyph.package), tone_pen(Tone::heading)});
    heading.push_back({"  not installed", tone_pen(Tone::note)});
    put_spans(screen, 1, 0, heading, size.cols);

    const unsigned first = 3;
    const unsigned height = size.rows - first - 1;
    app.set_height(height);
    for (unsigned line = 0; line < height; ++line) {
        const auto index = listing.cursor.top + line;
        if (index >= listing.rows.size()) {
            break;
        }
        const auto& row = listing.rows.at(index);
        const unsigned at = first + line;
        if (row.type == RowType::heading) {
            put_spans(screen, at, 1, {{row.text, tone_pen(Tone::heading)}}, size.cols);
        } else if (row.type == RowType::version && row.version) {
            const bool selected = index == listing.cursor.at;
            const auto bg = selected ? std::optional<Color>{palette::surface} : std::nullopt;
            if (selected) {
                screen.fill_row(at, {.fg = std::nullopt, .bg = palette::surface});
            }
            std::vector<Span> spans{marker(selected, glyph)};
            std::ranges::move(version_spans(*row.version, glyph), std::back_inserter(spans));
            put_spans(screen, at, 0, spans, size.cols, bg);
        } else {
            put_spans(screen, at, 3, {{row.text, tone_pen(Tone::note)}}, size.cols);
        }
    }
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
    const auto shown = std::min<std::size_t>(dialog.lines.size(), size.rows - 4);
    const auto top = std::min(dialog.top, dialog.lines.size() - shown);
    std::string closing = !dialog.closing.empty() ? dialog.closing
                          : dialog.question       ? " y yes  n no "
                                                  : " any key ";
    if (shown < dialog.lines.size()) {
        closing = std::format(" {} scroll {}", glyph.move, closing.substr(1));
    }
    std::string title = dialog.error ? std::format(" {} {} ", glyph.broken, dialog.title)
                                     : std::format(" {} ", dialog.title);
    std::size_t widest = columns(title) + columns(closing);
    for (const auto& line : dialog.lines) {
        widest = std::max(widest, columns(line) + 4);
    }
    const auto width = std::min<std::size_t>(widest + 2, size.cols - 2);
    const auto inner = width - 2;
    const auto height = shown + 4;
    const auto row = static_cast<unsigned>((size.rows - height) / 2);
    const auto left = static_cast<unsigned>((size.cols - width) / 2);
    const auto right = static_cast<unsigned>(left + width);

    title = clip(title, inner - 1);
    put_spans(screen, row, left,
              {{std::format("{}{}", glyph.frame.top_left, glyph.frame.across), edge},
               {title, title_pen},
               {std::format("{}{}", repeat(glyph.frame.across, inner - 1 - columns(title)),
                            glyph.frame.top_right),
                edge}},
              right);
    const auto blank = [&](unsigned at, std::string_view text) {
        const auto cut = clip(text, inner - 4);
        put_spans(screen, at, left,
                  {{std::string{glyph.frame.down}, edge},
                   {std::format("  {}{}", cut, std::string(inner - 2 - columns(cut), ' ')), body},
                   {std::string{glyph.frame.down}, edge}},
                  right);
    };
    blank(row + 1, "");
    for (std::size_t line = 0; line < shown; ++line) {
        blank(row + 2 + static_cast<unsigned>(line), dialog.lines.at(top + line));
    }
    blank(static_cast<unsigned>(row + height - 2), "");
    const auto key_room = std::min(columns(closing), inner - 1);
    put_spans(
        screen, static_cast<unsigned>(row + height - 1), left,
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
        {std::format(" {} {} {}", glyph.package,
                     snapshot.publisher == emerge::Publisher::exec ? "egraph exec" : "emerge",
                     snapshot.pid),
         tone_pen(Tone::heading)},
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
        put_spans(
            screen, 1, 1,
            {{"No emerge or egraph exec is publishing its progress", tone_pen(Tone::heading)}},
            size.cols);
        put_spans(screen, 3, 3,
                  {{"emerge publishes it to /run/portage with FEATURES=\"observability\",",
                    tone_pen(Tone::note)}},
                  size.cols);
        put_spans(screen, 4, 3,
                  {{"egraph exec to /run/egraph; this view reads them again every second.",
                    tone_pen(Tone::note)}},
                  size.cols);
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
        } else if (app.listing()) {
            draw_listing(screen, app, glyph, size);
        } else if (app.output()) {
            draw_output(screen, app, glyph, size);
        } else if (app.search()) {
            draw_search(screen, app, glyph, size);
        } else if (app.checked()) {
            draw_check(screen, app, glyph, size);
        } else if (app.watched()) {
            draw_watch(screen, app, glyph, size);
        } else if (app.planned()) {
            draw_plan(screen, app, glyph, size);
        } else if (app.on_notices()) {
            draw_notices(screen, app, glyph, size);
        } else {
            draw_list(screen, app, glyph, size);
        }
        if (app.prompt() || app.command_requested()) {
            draw_prompt(screen, app, size);
        } else {
            draw_hints(screen, size.rows - 1, size.cols, view_keys(app, glyph));
        }
        app.set_dialog_height(size.rows - 4);
        if (app.dialog()) {
            draw_dialog(screen, *app.dialog(), glyph, size);
        } else if (app.putting_off()) {
            draw_dialog(screen, put_off_dialog(), glyph, size);
        } else if (app.keys_shown()) {
            draw_dialog(screen, keys_dialog(view_keys(app, glyph)), glyph, size);
        } else if (const auto& action = app.preview_requested()) {
            std::string command = "egraph";
            for (const auto& word : action_arguments(*action)) {
                command += ' ' + word;
            }
            draw_dialog(screen,
                        {.error = false,
                         .title = "Planning",
                         .lines = {command},
                         .question = false,
                         .top = 0},
                        glyph, size);
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
// inputs every stale_interval, refreshing them in the background once they changed. Stopping
// early, why: the terminal could not be taken back from a program the interface stepped aside for.
template <class S>
std::optional<std::string> run(S& screen, App& app, const Glyphs& glyph, const Services& services) {
    const auto now = [&services] {
        return services.now ? services.now() : std::chrono::steady_clock::now();
    };
    const bool watches = services.stale && services.refresh && app.shared();
    auto next_check = now() + stale_interval;
    std::optional<std::chrono::steady_clock::time_point> next_watch;
    std::optional<Job<CheckResult>> check;
    std::optional<Job<RebuildResult>> rebuild;
    std::optional<Job<RefreshResult>> refresh;
    std::optional<Job<IndexResult>> index;
    std::optional<Job<RunResult>> running;
    std::optional<Job<ScopePlan>> scoped;
    std::optional<Job<TryResult>> tried;
    const auto read_status = [&] {
        if (services.status) {
            app.finish_status(services.status());
        }
        if (services.notices) {
            app.finish_notices(services.notices());
        }
    };
    read_status();
    auto next_status = now() + stale_interval;
    draw(screen, app, glyph);
    while (!app.done()) {
        const auto refreshed = poll(
            refresh, app.refresh_requested(), [&] { return services.refresh(); },
            [&](RefreshResult result) {
                app.finish_refresh(std::move(result));
                next_check = now() + (app.refresh_error() ? retry_interval : stale_interval);
            });
        const auto indexed = poll(
            index, app.index_requested(),
            [&] {
                return services.load_index
                           ? services.load_index()
                           : ready(IndexResult{std::unexpected(std::string{
                                 "this egraph has no way to load the repository index"})});
            },
            [&](IndexResult result) { app.finish_index(std::move(result)); });
        const auto ran = poll(
            running, app.run_requested().has_value(),
            [&] {
                return services.run
                           ? services.run(*app.run_requested())
                           : ready(RunResult{.error = "this egraph has no way to run actions"});
            },
            [&](RunResult result) { app.finish_run(std::move(result)); });
        const auto planned = poll(
            scoped, app.scope_plan_requested(), [&] { return app.start_scope_plan(); },
            [&](ScopePlan plan) { app.finish_scope_plan(std::move(plan)); });
        const auto tried_lines = poll(
            tried, app.try_requested(),
            [&] {
                auto request = app.start_try();
                return services.try_lines ? services.try_lines(request)
                                          : ready(TryResult{std::unexpected(std::string{
                                                "this egraph has no way to try configuration"})});
            },
            [&](TryResult result) { app.finish_try(std::move(result)); });
        if (now() >= next_status) {
            next_status = now() + stale_interval;
            read_status();
        }
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
            refreshed == Polled::finished || indexed == Polled::finished ||
            ran == Polled::finished || planned == Polled::finished ||
            tried_lines == Polled::finished || found_stale) {
            // Drawn below before any key is read.
        } else if (const auto change = app.steve_change_requested()) {
            app.finish_steve_change(services.set_steve
                                        ? services.set_steve(change->setting, change->value)
                                        : std::unexpected(std::string{"no way to change steve"}));
        } else if (const auto& notice = app.notice_change_requested(); notice && notice->read) {
            app.finish_notice_change(
                services.mark_read
                    ? services.mark_read(notice->notice)
                    : std::unexpected(std::string{"this egraph has no way to mark news read"}));
        } else if (notice) {
            app.finish_notice_change(
                services.set_aside
                    ? services.set_aside(notice->notice, notice->later)
                    : std::unexpected(std::string{"this egraph has no way to set notices aside"}));
        } else if (const auto& news = app.news_requested()) {
            app.finish_news(services.news_text
                                ? services.news_text(*news)
                                : std::unexpected(std::string{"this egraph cannot read news"}));
        } else if (app.dispatch_conf_requested()) {
            if (!services.dispatch_conf) {
                app.finish_dispatch_conf(
                    std::unexpected(std::string{"this egraph has no way to run dispatch-conf"}));
            } else {
                screen.suspend();
                const auto exited = services.dispatch_conf();
                if (auto resumed = screen.resume(); !resumed) {
                    return std::move(resumed.error());
                }
                app.finish_dispatch_conf(exited);
                read_status();
            }
        } else if (app.watch_requested() &&
                   (app.watched()->urgent || !next_watch || now() >= *next_watch)) {
            next_watch = now() + watch_interval;
            app.finish_watch(services.watch ? services.watch() : std::vector<emerge::Snapshot>{},
                             services.sample ? services.sample() : pressure::Sample{},
                             services.steve ? services.steve() : std::nullopt,
                             services.merge_list ? services.merge_list()
                                                 : std::vector<emerge::Pending>{});
        } else if (const auto& action = app.preview_requested()) {
            app.finish_preview(services.preview
                                   ? services.preview(*action)
                                   : Preview{.ready = false,
                                             .out = {},
                                             .err = "this egraph has no way to run actions"});
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
            if (app.watch_requested() && next_watch) {
                const auto due =
                    std::max(std::chrono::ceil<std::chrono::milliseconds>(*next_watch - now()),
                             std::chrono::milliseconds{0});
                timeout = timeout ? std::min(*timeout, due) : due;
            }
            app.handle(screen.read(timeout));
        }
        if (!app.done()) {
            draw(screen, app, glyph);
        }
    }
    return std::nullopt;
}

// Opens the terminal and runs the interface over the stores, first showing any warnings from
// opening them, on the notices page where asked; errors go to err.
[[nodiscard]] Exit open_and_run(std::shared_ptr<const Stores> stores, bool dynamic_deps,
                                GlyphSet glyphs, const Services& services,
                                std::span<const std::string> warnings, bool notices_first,
                                std::ostream& err);

} // namespace egraph::tui
