#pragma once

#include "evaluated.hpp"
#include "human.hpp"
#include "job.hpp"
#include "query.hpp"
#include "verify.hpp"

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
};

struct Soname {
    static constexpr std::string_view name = "soname";
    std::string soname;
    bool providers = false;
};

struct Broken {
    static constexpr std::string_view name = "broken";
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
    // Also the updates installed dependents hold back, and which atoms do.
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
    // In merge order, with what each waits for.
    bool table = false;
    // The plan held to emerge --pretend's.
    bool verify = false;
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

// What needs the user once emerge has run: configuration updates waiting, unread news.
struct NoticesCommand {
    static constexpr std::string_view name = "notices";
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
};

struct Stats {
    static constexpr std::string_view name = "stats";
};

struct Rebuild {
    static constexpr std::string_view name = "rebuild";
};

// Brings the store up to date, as any query does first, and answers nothing.
struct Refresh {
    static constexpr std::string_view name = "refresh";
};

struct Check {
    static constexpr std::string_view name = "check";
};

struct Tui {
    static constexpr std::string_view name = "tui";
};

// Commands read one per line, answered from one session.
struct Shell {
    static constexpr std::string_view name = "shell";
};

// For portage's neighborhood completion: see affected.hpp.
struct Affected {
    static constexpr std::string_view name = "affected";
    // The JSON request; - is standard input.
    std::string request = "-";
};

using Command =
    std::variant<std::monostate, Deps, Rdeps, Why, Match, Soname, Broken, Blockers, Orphans,
                 Updates, PlanCommand, Update, Install, Remove, Select, Deselect, NoticesCommand,
                 Export, Stats, Rebuild, Refresh, Check, Tui, Shell, Affected>;

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
    // Run to refresh a stale store; unset, the egraph-build next to egraph, else the one in PATH.
    std::optional<std::string> builder;
    // Run to verify a plan; unset, the emerge in PATH.
    std::optional<std::string> emerge;
    // Offered after an action leaves configuration updates; unset, the dispatch-conf in PATH.
    std::optional<std::string> dispatch_conf;
    bool no_refresh = false;
    // emerge --dynamic-deps: dependency queries read an installed package's dependencies from its
    // ebuild when the same version is still in its repository (the evaluated store).
    bool dynamic_deps = true;
    Layout layout = Layout::automatic;
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
    // USE changes were offered already, so they are not again when the command runs anew.
    bool use_offered = false;
    // The rebuild of what uses preserved libraries was offered already, by the action that runs.
    bool rebuild_offered = false;
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

// The egraph-build command line that writes what each merge list entry ("ebuild:cpv" or
// "binary:cpv") waits for to output.
[[nodiscard]] std::vector<std::string> pending_command(const Invocation& invocation,
                                                       const std::filesystem::path& output,
                                                       const std::vector<std::string>& entries);

// The egraph-build command line that writes EMERGE_DEFAULT_OPTS, a word a line, to output.
[[nodiscard]] std::vector<std::string> emerge_options_command(const Invocation& invocation,
                                                              const std::filesystem::path& output);

// The egraph-build command line that writes the notices, as JSON, to output.
[[nodiscard]] std::vector<std::string> notices_command(const Invocation& invocation,
                                                       const std::filesystem::path& output);

// The dispatch-conf command line, under the invocation's roots.
[[nodiscard]] std::vector<std::string> dispatch_conf_command(const Invocation& invocation);

// The emerge command line that pretends to carry out request under the invocation's roots.
[[nodiscard]] std::vector<std::string> emerge_command(const Invocation& invocation,
                                                      const EmergeRequest& request);

// The emerge command line with arguments under the invocation's roots.
[[nodiscard]] std::vector<std::string> emerge_command(const Invocation& invocation,
                                                      std::span<const std::string> arguments);

// Declares every option and subcommand on app; app.parse() then fills invocation, which must
// outlive the parse.
void configure(CLI::App& app, Invocation& invocation);

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
