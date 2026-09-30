#pragma once

#include "evaluated.hpp"
#include "human.hpp"
#include "query.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <optional>
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
    // Only the packages the root sets reach, as emerge -uD @world.
    bool world = false;
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

using Command = std::variant<std::monostate, Deps, Rdeps, Why, Match, Soname, Broken, Orphans,
                             Updates, Export, Stats, Rebuild, Refresh, Check, Tui, Shell, Affected>;

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
    // Where the running egraph is, and the user's cache directory ($XDG_CACHE_HOME, or
    // ~/.cache), which main fills in.
    std::filesystem::path program_dir;
    std::optional<std::filesystem::path> cache_home;
    Command command;
};

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
// they were built from has changed since, else a full build's.
[[nodiscard]] std::expected<Stores, std::string>
save_stores(const Invocation& invocation, const std::optional<ScratchStores>& checked);

// The egraph-build command to run.
[[nodiscard]] std::string builder_program(const Invocation& invocation);

// The egraph-build command line that writes the store at path; mode is --full or
// --incremental.
[[nodiscard]] std::vector<std::string> builder_command(const Invocation& invocation,
                                                       std::string_view mode,
                                                       const std::filesystem::path& path);

// The egraph-build command line that writes what each merge list entry ("ebuild:cpv" or
// "binary:cpv") waits for to output.
[[nodiscard]] std::vector<std::string> pending_command(const Invocation& invocation,
                                                       const std::filesystem::path& output,
                                                       const std::vector<std::string>& entries);

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
