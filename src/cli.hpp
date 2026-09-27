#pragma once

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
};

struct Deps {
    static constexpr std::string_view name = "deps";
    std::string package;
};

struct Rdeps {
    static constexpr std::string_view name = "rdeps";
    std::string package;
};

struct Why {
    static constexpr std::string_view name = "why";
    std::string package;
};

struct Soname {
    static constexpr std::string_view name = "soname";
    std::string soname;
};

struct Broken {
    static constexpr std::string_view name = "broken";
};

struct Orphans {
    static constexpr std::string_view name = "orphans";
};

enum class ExportFormat : std::uint8_t { dot, json };

struct Export {
    static constexpr std::string_view name = "export";
    ExportFormat format = ExportFormat::dot;
    std::vector<std::string> packages;
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

using Command = std::variant<std::monostate, Deps, Rdeps, Why, Soname, Broken, Orphans, Export,
                             Stats, Rebuild, Check>;

struct Invocation {
    // Both are handed to egraph-build, which evaluates packages under root with the portage
    // configuration under config_root.
    std::filesystem::path root = "/";
    std::filesystem::path config_root = "/";
    std::optional<std::filesystem::path> store;
    bool no_refresh = false;
    Command command;
};

// Declares every option and subcommand on app; app.parse() then fills invocation, which must
// outlive the parse.
void configure(CLI::App& app, Invocation& invocation);

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err);

} // namespace egraph
