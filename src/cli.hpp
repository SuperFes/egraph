#pragma once

#include "complete.hpp"
#include "evaluated.hpp"
#include "human.hpp"
#include "job.hpp"
#include "log.hpp"
#include "query.hpp"
#include "search.hpp"
#include "verify.hpp"
#include "what_if.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace CLI {
class App;
}

namespace egraph {

enum class Exit : std::uint8_t {
    ok = 0,
    failure = 1,
    usage = 2,
    not_implemented = 3,
    // egraph check: the store differs from a fresh build.
    drift = 4,
    // --verify, or an action before merging: emerge --pretend would merge otherwise.
    differs = 5,
    // updates, plan and the actions: emerge would refuse the plan, for blockers it cannot
    // resolve, dependencies nothing satisfies, REQUIRED_USE unmet or USE changes it needs.
    refused = 6,
    // Never a status egraph exits with: an action shown for the interface, ready to run.
    previewed = 7,
};

struct Deps {
    static constexpr std::string_view name = "deps";
    std::vector<std::string> packages;
    // Also the dependencies the ebuilds would add with flags toggled.
    bool possible = false;
};

struct Rdeps {
    static constexpr std::string_view name = "rdeps";
    std::vector<std::string> packages;
    // Also the dependencies the ebuilds would add with flags toggled.
    bool possible = false;
};

struct Why {
    static constexpr std::string_view name = "why";
    std::string package;
    // emerge --with-bdeps: whether build-time dependencies keep packages.
    bool build_deps = true;
};

struct Match {
    static constexpr std::string_view name = "match";
    std::vector<std::string> atoms;
    // The installed cps' ebuilds instead, with the USE each would be built with now.
    bool candidates = false;
    // Atoms of configuration files, wildcards and all, matched against the ebuilds, each
    // ebuild's in the order portage applies their entries.
    bool config = false;
};

// Each flag's state for the ebuilds a configuration atom matches, and where it was set.
struct UseCommand {
    static constexpr std::string_view name = "use";
    std::string package;
    std::optional<std::string> flag;
    // Every ebuild the atom matches, not only what emerge would build and the installed ones.
    bool all = false;
};

struct Soname {
    static constexpr std::string_view name = "soname";
    std::string soname;
    bool providers = false;
};

struct Broken {
    static constexpr std::string_view name = "broken";
};

// emerge --search over the repositories and the installed packages.
struct Search {
    static constexpr std::string_view name = "search";
    std::vector<std::string> keys;
    SearchOptions options;
};

// Every version in the repositories of packages, and why each masked one is.
struct Versions {
    static constexpr std::string_view name = "versions";
    std::vector<std::string> packages;
};

enum class ConfigAction : std::uint8_t { check };

// The user's configuration: check lists the entries that do nothing or that later ones undo.
struct ConfigCommand {
    static constexpr std::string_view name = "config";
    ConfigAction action = ConfigAction::check;
};

struct Blockers {
    static constexpr std::string_view name = "blockers";
    std::vector<std::string> packages;
};

struct Orphans {
    static constexpr std::string_view name = "orphans";
    // emerge --with-bdeps: whether build-time dependencies keep packages.
    bool build_deps = true;
};

struct Updates {
    static constexpr std::string_view name = "updates";
    UseRebuilds rebuilds = UseRebuilds::none;
    // Also the updates installed dependents hold back, and which atoms do; those nothing can
    // satisfy are always listed.
    bool held = false;
    // In merge order, with what each waits for.
    bool table = false;
    // Each merge under the root and packages it comes from.
    bool tree = false;
    // Only the packages the root sets reach, as emerge -u @world.
    bool world = false;
    // As emerge --deep.
    bool deep = false;
    // The plan held to emerge --pretend's.
    bool verify = false;
    // Where the plan goes as mtimedb's resume entry.
    std::optional<std::filesystem::path> resume_list;
    // Where the plan goes as egraph-build --worker's requests.
    std::optional<std::filesystem::path> requests;
    // With --env lines tried, the installed packages they build otherwise rebuilt, as emerge
    // --reinstall-atoms rebuilds them.
    bool rebuild_env = false;
};

// A request planned as emerge --pretend would merge it.
struct PlanCommand {
    static constexpr std::string_view name = "plan";
    // Atoms and sets, as emerge's arguments.
    std::vector<std::string> targets;
    UseRebuilds rebuilds = UseRebuilds::none;
    // As emerge -u, --deep and --noreplace.
    bool update = false;
    bool deep = false;
    bool noreplace = false;
    // Every held update, as updates --held lists them, rather than only those nothing can satisfy.
    bool held = false;
    // In merge order, with what each waits for.
    bool table = false;
    // The plan held to emerge --pretend's.
    bool verify = false;
    // Where the plan goes as mtimedb's resume entry.
    std::optional<std::filesystem::path> resume_list;
    // Where the plan goes as egraph-build --worker's requests.
    std::optional<std::filesystem::path> requests;
    // With --env lines tried, the installed packages they build otherwise rebuilt, as emerge
    // --reinstall-atoms rebuilds them.
    bool rebuild_env = false;
};

// emerge -u run on the updates once shown and confirmed, as emerge --oneshot: what it merges
// joins no set.
struct Update : Updates {
    static constexpr std::string_view name = "update";
    // Run without asking.
    bool yes = false;
};

// emerge run on a request once shown and confirmed.
struct Install : PlanCommand {
    static constexpr std::string_view name = "install";
    // As emerge --oneshot: the targets join no set.
    bool oneshot = false;
    // Run without asking.
    bool yes = false;
};

// A request carried out by egraph-build --worker, one package at a time, once shown, verified
// and confirmed, as install has emerge carry it out.
struct Exec : Install {
    static constexpr std::string_view name = "exec";
    // Builds at once, as emerge --jobs; none for EMERGE_DEFAULT_OPTS' count.
    std::optional<std::uint32_t> jobs;
    // Where to write when each step starts and ends.
    std::optional<std::filesystem::path> trace;
    // Go on after a failure, as emerge --keep-going; none for EMERGE_DEFAULT_OPTS'.
    std::optional<bool> keep_going;
    // The portage fork's --merge-wait-scope; none for EMERGE_DEFAULT_OPTS'.
    std::optional<std::string> merge_wait_scope;
    // Carry out the last run's request again, without what it merged.
    bool resume = false;
};

// emerge --depclean run on packages once shown, verified and confirmed.
struct Remove {
    static constexpr std::string_view name = "remove";
    std::vector<std::string> packages;
    // emerge --with-bdeps: whether build-time dependencies keep packages.
    bool build_deps = true;
    // Run without asking.
    bool yes = false;
};

// emerge --select --noreplace run on packages once shown and confirmed.
struct Select {
    static constexpr std::string_view name = "select";
    std::vector<std::string> packages;
    bool yes = false;
};

// emerge --deselect run on packages once shown and confirmed.
struct Deselect {
    static constexpr std::string_view name = "deselect";
    std::vector<std::string> packages;
    bool yes = false;
};

// emaint sync --auto (or the repositories named, as emerge --sync takes them), then the updates the
// repositories now offer, shown as updates shows them, and the notices.
struct Sync : Updates {
    static constexpr std::string_view name = "sync";
    std::vector<std::string> repositories;
};

// How long a notice is put off for.
enum class PutOff : std::uint8_t { hour, day, week };

// What needs the user: GLSAs, configuration updates waiting, unread news and the rest.
struct NoticesCommand {
    static constexpr std::string_view name = "notices";
    // Notices to dismiss, or to put off for put_off, by name (named_notice).
    std::vector<std::string> dismiss;
    std::vector<std::string> later;
    PutOff put_off = PutOff::day;
    // Those set aside too.
    bool all = false;
};

enum class ExportFormat : std::uint8_t { dot, json };

struct Export {
    static constexpr std::string_view name = "export";
    ExportFormat format = ExportFormat::dot;
    std::vector<std::string> packages;
    std::uint32_t depth = 1;
    Direction direction = Direction::reverse;
    // The evaluated store, whole, instead of the installed one.
    bool evaluated = false;
    // The repository index, whole.
    bool repository = false;
};

struct Stats {
    static constexpr std::string_view name = "stats";
};

// The runs logged where --log writes, or one run's events.
struct LogCommand {
    static constexpr std::string_view name = "log";
    // A run's id, or the start of it.
    std::optional<std::string> run;
};

// The installed packages and root sets now against a generation of the system store's history.
struct Diff {
    static constexpr std::string_view name = "diff";
    // An age, a date or a generation's name; the newest generation without one.
    std::optional<std::string> base;
    bool json = false;
};

// The updates egraph watch last planned, from the status file.
struct StatusCommand {
    static constexpr std::string_view name = "status";
    bool json = false;
    // Plan them and write the status file first.
    bool update = false;
};

// The system store's history log, or a package's events and what pulled it in.
struct HistoryCommand {
    static constexpr std::string_view name = "history";
    // An age or a date, and packages.
    std::vector<std::string> arguments;
};

struct Rebuild {
    static constexpr std::string_view name = "rebuild";
};

// Brings the store up to date, as any query does first, and answers nothing.
struct Refresh {
    static constexpr std::string_view name = "refresh";
};

// Keeps the stores and the repository index fresh as their inputs change, until stopped.
struct Watch {
    static constexpr std::string_view name = "watch";
};

struct Check {
    static constexpr std::string_view name = "check";
};

// Keeps a desktop notification summing up the notices up, until stopped.
struct Notify {
    static constexpr std::string_view name = "notify";
};

struct Tui {
    static constexpr std::string_view name = "tui";
    // Open on the notices page.
    bool notices = false;
};

// Commands read one per line, answered from one session.
struct Shell {
    static constexpr std::string_view name = "shell";
};

// The words a shell completes a package argument to, from the stores as they are: never refreshed,
// as a key press must not start a build.
struct Complete {
    static constexpr std::string_view name = "complete";
    Completing what = Completing::atoms;
    std::string word;
};

// For portage's neighborhood completion: see affected.hpp.
struct Affected {
    static constexpr std::string_view name = "affected";
    // The JSON request; - is standard input.
    std::string request = "-";
};

using Command =
    std::variant<std::monostate, Deps, Rdeps, Why, Match, UseCommand, Soname, Broken, Search,
                 Versions, ConfigCommand, Blockers, Orphans, Updates, PlanCommand, Update, Install,
                 Exec, Remove, Select, Deselect, Sync, NoticesCommand, Export, Stats, LogCommand,
                 Diff, HistoryCommand, StatusCommand, Rebuild, Refresh, Watch, Check, Notify, Tui,
                 Shell, Complete, Affected>;

// How query results are written: for people (grouped, aligned, perhaps coloured) or as
// tab-separated lines for scripts. auto picks people on a terminal.
enum class Layout : std::uint8_t { automatic, human, lines };
enum class ColorMode : std::uint8_t { automatic, always, never };

struct Invocation {
    // Handed to egraph-build, which evaluates the packages under root with the portage
    // configuration under config_root. Unset ones are left to portage's defaults.
    std::filesystem::path root = "/";
    std::optional<std::filesystem::path> config_root;
    std::optional<std::filesystem::path> eprefix;
    std::optional<std::filesystem::path> store;
    // The replace-slots list; unset, the configuration root's.
    std::optional<std::filesystem::path> replace_slots;
    // Run to refresh a stale store; unset, the egraph-build next to egraph, else the one in PATH.
    std::optional<std::string> builder;
    // Run to verify a plan; unset, the emerge in PATH.
    std::optional<std::string> emerge;
    // Offered after an action leaves configuration updates; unset, the dispatch-conf in PATH.
    std::optional<std::string> dispatch_conf;
    // Run by sync; unset, the emaint in PATH.
    std::optional<std::string> emaint;
    bool no_refresh = false;
    // emerge --dynamic-deps: dependency queries read an installed package's dependencies from its
    // ebuild when the same version is still in its repository (the evaluated store).
    bool dynamic_deps = true;
    Layout layout = Layout::automatic;
    // Where runs that change the system are logged, and the file when there.
    log::Sink log = log::Sink::automatic;
    std::optional<std::filesystem::path> log_file;
    ColorMode color = ColorMode::automatic;
    // Unset, a Nerd Font's in a UTF-8 locale and ASCII otherwise.
    std::optional<GlyphSet> glyphs;
    // Facts about where output goes, which main fills in: stdout is a terminal, NO_COLOR is set
    // to something, COLORTERM says the terminal takes 24-bit colour, and the locale's character
    // set is UTF-8.
    bool terminal = false;
    bool no_color = false;
    bool truecolor = false;
    bool utf8 = false;
    // Standard input is a terminal, so the shell prompts.
    bool input_terminal = false;
    // A command may ask a question on standard input: one given on the command line with both
    // ends a terminal, never a shell or interface line.
    bool ask = false;
    // An action is only shown, to be confirmed by the interface (Exit::previewed).
    bool preview = false;
    // USE changes were offered already, so they are not again when the command runs anew.
    bool use_offered = false;
    // The rebuild of what uses preserved libraries was offered already, by the action that runs.
    bool rebuild_offered = false;
    // Configuration lines tried (--use, --env): queries and plans answer as if egraph's own
    // files held them; actions refuse them.
    std::vector<WhatIfLine> what_if;
    // Where the running egraph is, and the user's cache directory ($XDG_CACHE_HOME, or
    // ~/.cache), which main fills in.
    std::filesystem::path program_dir;
    std::optional<std::filesystem::path> cache_home;
    Command command;
};

// Asks question on out and reads the answer from in: yes only for y or yes; a closed input is no.
[[nodiscard]] bool answered_yes(std::istream& in, std::ostream& out, std::string_view question);

// The layout, colouring and glyphs an invocation resolves to. Lines are never coloured.
struct Style {
    bool human = false;
    ColorDepth color = ColorDepth::none;
    GlyphSet glyphs = GlyphSet::nerd;
};
[[nodiscard]] Style style(const Invocation& invocation);

// Where portage reads its configuration: --config-root, else ${EPREFIX}, else /.
[[nodiscard]] std::filesystem::path config_root(const Invocation& invocation);

// The store under the root: ${ROOT}${EPREFIX}/var/cache/egraph/installed.egraph.
[[nodiscard]] std::filesystem::path system_store_path(const Invocation& invocation);

// The user's own store for the root, in the cache directory; nullopt without one.
[[nodiscard]] std::optional<std::filesystem::path> user_store_path(const Invocation& invocation);

// The store to build or refresh: --store, else the system store when this process can write it,
// else the user's.
[[nodiscard]] std::filesystem::path store_path(const Invocation& invocation);

// Store files a build wrote at a scratch path, the evaluated one beside it; removed with this.
class ScratchStores {
  public:
    explicit ScratchStores(std::filesystem::path path) : path_{std::move(path)} {}
    ScratchStores(const ScratchStores&) = delete;
    ScratchStores& operator=(const ScratchStores&) = delete;
    ScratchStores(ScratchStores&& other) noexcept;
    ScratchStores& operator=(ScratchStores&& other) noexcept;
    ~ScratchStores();

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    void remove() const;

    std::filesystem::path path_;
};

// Saves the stores at store_path(invocation) and loads them: copies of a check's when nothing
// they were built from has changed since, else a full build's, made in the background.
[[nodiscard]] Job<std::expected<Stores, std::string>>
save_stores(const Invocation& invocation, const std::optional<ScratchStores>& checked);

// The egraph-build command to run.
[[nodiscard]] std::string builder_program(const Invocation& invocation);

// The egraph-build command line that writes the store at path; mode is --full, --incremental,
// or --evaluate with the cps to evaluate.
[[nodiscard]] std::vector<std::string> builder_command(const Invocation& invocation,
                                                       std::string_view mode,
                                                       const std::filesystem::path& path,
                                                       std::span<const std::string> cps = {});

// The egraph-build --worker command line, under the invocation's roots.
[[nodiscard]] std::vector<std::string> worker_command(const Invocation& invocation);

// The egraph-build command line that writes what each merge list entry ("ebuild:cpv" or
// "binary:cpv") waits for to output.
[[nodiscard]] std::vector<std::string> pending_command(const Invocation& invocation,
                                                       const std::filesystem::path& output,
                                                       const std::vector<std::string>& entries);

// The egraph-build command line that writes the kernel source directories each installed cpv
// owns to output.
[[nodiscard]] std::vector<std::string> kernel_sources_command(const Invocation& invocation,
                                                              const std::filesystem::path& output,
                                                              const std::vector<std::string>& cpvs);

// The egraph-build command line that writes EMERGE_DEFAULT_OPTS, a word a line, to output.
[[nodiscard]] std::vector<std::string> emerge_options_command(const Invocation& invocation,
                                                              const std::filesystem::path& output);

// The egraph-build command line that marks a news item, repo/item, read.
[[nodiscard]] std::vector<std::string> news_read_command(const Invocation& invocation,
                                                         std::string_view item);

// The egraph-build command line that writes the notices, as JSON, to output.
[[nodiscard]] std::vector<std::string> notices_command(const Invocation& invocation,
                                                       const std::filesystem::path& output);

// The egraph-build command line that copies the running portage to directory, for workers to run
// from (worker_command and --portage-copy) while a run merges a new one.
[[nodiscard]] std::vector<std::string> copy_portage_command(const Invocation& invocation,
                                                            const std::filesystem::path& directory);

// exec's own arguments for command, as the run's record keeps them: the options that shape its
// plan and run, then its targets.
[[nodiscard]] std::vector<std::string> exec_arguments(const Exec& command);

// The command exec --resume carries out: the recorded run's arguments, with the --jobs,
// --keep-going and --merge-wait-scope given now in place of the run's, and given's --yes and
// --trace. An error for a record exec cannot parse.
[[nodiscard]] std::expected<Exec, std::string> resumed_exec(const Exec& given,
                                                            std::span<const std::string> arguments);

// The dispatch-conf command line, under the invocation's roots.
[[nodiscard]] std::vector<std::string> dispatch_conf_command(const Invocation& invocation);

// The emaint command line that syncs repositories (none: those set to auto-sync), under the
// invocation's roots.
[[nodiscard]] std::vector<std::string> sync_command(const Invocation& invocation,
                                                    const std::vector<std::string>& repositories);

// The emerge command line that pretends to carry out request under the invocation's roots.
[[nodiscard]] std::vector<std::string> emerge_command(const Invocation& invocation,
                                                      const EmergeRequest& request);

// The emerge command line with arguments under the invocation's roots.
[[nodiscard]] std::vector<std::string> emerge_command(const Invocation& invocation,
                                                      std::span<const std::string> arguments);

// The global options an egraph run beside this one takes to work on the same system, with the
// same programs and logs, as this one does.
[[nodiscard]] std::vector<std::string> egraph_options(const Invocation& invocation);

// Declares every option and subcommand on app; app.parse() then fills invocation, which must
// outlive the parse.
void configure(CLI::App& app, Invocation& invocation);

// The arguments to parse for egraph run as program: as egraph-<command> (a link to egraph, say),
// <command> first, for each of app's subcommands, which then takes the global options too; else
// arguments as they are.
[[nodiscard]] std::vector<std::string> multicall_arguments(CLI::App& app, std::string_view program,
                                                           std::vector<std::string> arguments);

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err);

// The shell: runs the commands read from in, one per line, against one session, until the end of
// input or "quit" (or "exit"). Each line takes the command-line syntax, global options included,
// except the ones that choose the stores (--root, --store and the like), which the session keeps.
// A line that fails is reported on err and the next one read; blank lines and "#" comments are
// skipped, and "help" prints the usage. With prompt, a prompt goes to out before each line. The
// status is the last command's.
Exit shell(const Invocation& invocation, std::istream& in, std::ostream& out, std::ostream& err,
           bool prompt);

} // namespace egraph
