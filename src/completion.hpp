#pragma once

// Shell completion scripts, generated from the command line's own definition so that they list
// exactly what it accepts.

#include <cstdint>
#include <string>
#include <vector>

namespace CLI {
class App;
}

namespace egraph {

enum class CompletionShell : std::uint8_t { bash, zsh, fish };

// What an option or argument takes, from its type name: nothing (a flag), any text, a directory,
// a file, a command, an installed package, or one of fixed choices.
enum class Takes : std::uint8_t { nothing, text, directory, file, command, package, choice };

// An option ("-t" and "--table"), or with no names a command's positional argument.
struct Completable {
    std::vector<std::string> names;
    std::string description;
    Takes takes = Takes::nothing;
    std::vector<std::string> choices;
};

struct CompletionCommand {
    std::string name;
    std::string description;
    std::vector<Completable> options;
    // Its positional arguments, the kind they take; nothing without any.
    Takes arguments = Takes::nothing;
};

// The global options, then the commands, in the order the command line declares them.
struct Completions {
    std::vector<Completable> options;
    std::vector<CompletionCommand> commands;
};

[[nodiscard]] Completions completions(const CLI::App& app);

// The script shell loads to complete egraph's command line: its commands, options, their fixed
// choices, and installed packages from the vdb under ${ROOT}.
[[nodiscard]] std::string completion_script(const Completions& completions, CompletionShell shell);

} // namespace egraph
