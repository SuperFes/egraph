#pragma once

#include "human.hpp"
#include "query.hpp"

#include <cstdint>
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
};

struct Rdeps {
    static constexpr std::string_view name = "rdeps";
    std::vector<std::string> packages;
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

enum class ExportFormat : std::uint8_t { dot, json };

struct Export {
    static constexpr std::string_view name = "export";
    ExportFormat format = ExportFormat::dot;
    std::vector<std::string> packages;
    std::uint32_t depth = 1;
    Direction direction = Direction::reverse;
};

struct Stats {
    static constexpr std::string_view name = "stats";
};

struct Rebuild {
    static constexpr std::string_view name = "rebuild";
};

struct Check {
    static constexpr std::string_view name = "check";
};

struct Tui {
    static constexpr std::string_view name = "tui";
};

using Command = std::variant<std::monostate, Deps, Rdeps, Why, Match, Soname, Broken, Orphans,
                             Export, Stats, Rebuild, Check, Tui>;

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
    // Run to refresh a stale store.
    std::string builder = "egraph-build";
    bool no_refresh = false;
    Layout layout = Layout::automatic;
    ColorMode color = ColorMode::automatic;
    GlyphSet glyphs = GlyphSet::nerd;
    // Facts about where output goes, which main fills in: stdout is a terminal, NO_COLOR is set
    // to something, and COLORTERM says the terminal takes 24-bit colour.
    bool terminal = false;
    bool no_color = false;
    bool truecolor = false;
    Command command;
};

// The layout and colouring an invocation resolves to. Lines are never coloured.
struct Style {
    bool human = false;
    ColorDepth color = ColorDepth::none;
};
[[nodiscard]] Style style(const Invocation& invocation);

[[nodiscard]] std::filesystem::path store_path(const Invocation& invocation);

// The egraph-build command line that writes the store at path; mode is --full or
// --incremental.
[[nodiscard]] std::vector<std::string> builder_command(const Invocation& invocation,
                                                       std::string_view mode,
                                                       const std::filesystem::path& path);

// Declares every option and subcommand on app; app.parse() then fills invocation, which must
// outlive the parse.
void configure(CLI::App& app, Invocation& invocation);

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err);

} // namespace egraph
