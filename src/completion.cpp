#include "completion.hpp"

#include <CLI/CLI.hpp>

#include <format>
#include <ranges>
#include <string_view>

namespace egraph {

namespace {

// What an option takes, from the type name configure() gives it and the choices one_of lists.
Takes takes_of(const CLI::Option& option, std::vector<std::string>& choices) {
    if (option.get_expected_max() == 0) {
        return Takes::nothing;
    }
    const auto type = option.get_type_name();
    // one_of's description, "{a,b,c}", follows the type after a colon.
    if (const auto open = type.find(":{"); open != std::string::npos && type.ends_with('}')) {
        const auto listed = std::string_view{type}.substr(open + 2, type.size() - open - 3);
        for (const auto choice : std::views::split(listed, ',')) {
            choices.emplace_back(std::string_view{choice});
        }
        return Takes::choice;
    }
    const auto base = type.substr(0, type.find(':'));
    if (base == "DIR") {
        return Takes::directory;
    }
    if (base == "FILE") {
        return Takes::file;
    }
    if (base == "COMMAND") {
        return Takes::command;
    }
    if (base == "PACKAGE") {
        return Takes::package;
    }
    if (base == "ATOM") {
        return Takes::atom;
    }
    if (base == "REPOSITORY") {
        return Takes::repository;
    }
    return Takes::text;
}

Completable completable(const CLI::Option& option) {
    Completable found{.names = {}, .description = option.get_description(), .choices = {}};
    for (const auto& name : option.get_snames()) {
        found.names.push_back("-" + name);
    }
    for (const auto& name : option.get_lnames()) {
        found.names.push_back("--" + name);
    }
    found.takes = takes_of(option, found.choices);
    return found;
}

std::vector<Completable> options_of(const CLI::App& app) {
    std::vector<Completable> options;
    for (const auto* option : app.get_options()) {
        if (option->nonpositional()) {
            options.push_back(completable(*option));
        }
    }
    return options;
}

// Options that take a value, which the scripts step over when looking for the command.
std::vector<std::string> valued(const std::vector<Completable>& options) {
    std::vector<std::string> names;
    for (const auto& option : options) {
        if (option.takes != Takes::nothing) {
            names.insert(names.end(), option.names.begin(), option.names.end());
        }
    }
    return names;
}

std::string joined(const std::vector<std::string>& words, std::string_view separator) {
    std::string out;
    for (const auto& word : words) {
        out += (out.empty() ? "" : std::string{separator}) + word;
    }
    return out;
}

std::vector<std::string> all_names(const std::vector<Completable>& options) {
    std::vector<std::string> names;
    for (const auto& option : options) {
        names.insert(names.end(), option.names.begin(), option.names.end());
    }
    return names;
}

// A case pattern matching every name of the option.
std::string alternatives(const Completable& option) {
    return joined(option.names, "|");
}

// egraph complete's option for what takes, which the scripts pass on.
std::string_view complete_option(Takes takes) {
    switch (takes) {
    case Takes::package:
        return " --installed";
    case Takes::repository:
        return " --repositories";
    case Takes::nothing:
    case Takes::text:
    case Takes::directory:
    case Takes::file:
    case Takes::command:
    case Takes::atom:
    case Takes::choice:
        break;
    }
    return "";
}

// bash: the line filling COMPREPLY with the words that start with $cur.
std::string bash_words(const std::vector<std::string>& words) {
    return std::format(R"(mapfile -t COMPREPLY < <(compgen -W "{}" -- "$cur"))",
                       joined(words, " "));
}

// bash: the line filling COMPREPLY with what takes offers for $cur.
std::string bash_values(Takes takes, const std::vector<std::string>& choices) {
    switch (takes) {
    case Takes::directory:
        return R"(compopt -o filenames; mapfile -t COMPREPLY < <(compgen -d -- "$cur"))";
    case Takes::file:
        return R"(compopt -o filenames; mapfile -t COMPREPLY < <(compgen -f -- "$cur"))";
    case Takes::command:
        return R"(mapfile -t COMPREPLY < <(compgen -c -- "$cur"))";
    case Takes::package:
    case Takes::atom:
    case Takes::repository:
        return std::format("_egraph_complete{}", complete_option(takes));
    case Takes::choice:
        return bash_words(choices);
    case Takes::nothing:
    case Takes::text:
        break;
    }
    return "COMPREPLY=()";
}

// The case arms completing the values of options, then options or the arguments.
void bash_scope(std::string& out, const std::vector<Completable>& options,
                const std::string& arguments, std::string_view indent) {
    out += std::format("{}case $prev in\n", indent);
    for (const auto& option : options) {
        if (option.takes != Takes::nothing) {
            out += std::format("{}    {}) {}; return ;;\n", indent, alternatives(option),
                               bash_values(option.takes, option.choices));
        }
    }
    out += std::format("{}esac\n", indent);
    out += std::format("{}if [[ $cur == -* ]]; then\n", indent);
    out += std::format("{}    {}\n", indent, bash_words(all_names(options)));
    out += std::format("{}else\n{}    {}\n{}fi\n", indent, indent, arguments, indent);
}

std::string bash_script(const Completions& completions) {
    std::string out = "# bash completion for egraph, generated from its command line.\n\n";
    out +=
        R"bash(# Package arguments, which egraph completes from its stores; the arguments say what they are.
_egraph_complete() {
    # COMP_WORDBREAKS splits a word at the = and : atoms hold, so egraph is given it whole.
    local word=$cur
    if [[ -n ${COMP_LINE-} ]]; then
        word=${COMP_LINE:0:COMP_POINT}
        word=${word##*[[:space:]]}
    fi
    local cut=${word%"$cur"}
    mapfile -t COMPREPLY < <(egraph complete "$@" -- "$word" 2>/dev/null)
    COMPREPLY=("${COMPREPLY[@]#"$cut"}")
    if [[ ${#COMPREPLY[@]} -eq 1 && ${COMPREPLY[0]} == */ ]]; then
        compopt -o nospace 2>/dev/null
    fi
    return 0
}

_egraph() {
    local cur=${COMP_WORDS[COMP_CWORD]} prev=${COMP_WORDS[COMP_CWORD - 1]}
    local command="" i
    for ((i = 1; i < COMP_CWORD; i++)); do
        case ${COMP_WORDS[i]} in
)bash";
    if (const auto names = valued(completions.options); !names.empty()) {
        out += std::format("            {}) ((i++)) ;;\n", joined(names, "|"));
    }
    out += R"(            -*) ;;
            *)
                command=${COMP_WORDS[i]}
                break
                ;;
        esac
    done
    case $command in
    "")
)";
    std::vector<std::string> names;
    names.reserve(completions.commands.size());
    for (const auto& command : completions.commands) {
        names.push_back(command.name);
    }
    bash_scope(out, completions.options, bash_words(names), "        ");
    out += "        ;;\n";
    for (const auto& command : completions.commands) {
        out += std::format("    {})\n", command.name);
        bash_scope(out, command.options, bash_values(command.arguments, {}), "        ");
        out += "        ;;\n";
    }
    out += "    esac\n}\n\ncomplete -F _egraph egraph\n";
    return out;
}

// Text inside a single-quoted zsh word, and inside an _arguments description's brackets.
std::string zsh_quoted(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\'') {
            out += R"('\'')";
        } else {
            out += c;
        }
    }
    return out;
}

std::string zsh_bracketed(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '[' || c == ']') {
            out += '\\';
        }
        out += c;
    }
    return zsh_quoted(out);
}

// _arguments' message and action for a value.
std::string zsh_action(Takes takes, const std::vector<std::string>& choices) {
    switch (takes) {
    case Takes::directory:
        return "directory:_files -/";
    case Takes::file:
        return "file:_files";
    case Takes::command:
        return "command:_command_names -e";
    case Takes::package:
        return "package:_egraph_complete installed";
    case Takes::atom:
        return "package:_egraph_complete atoms";
    case Takes::repository:
        return "repository:_egraph_complete repositories";
    case Takes::choice:
        return std::format("value:({})", joined(choices, " "));
    case Takes::nothing:
    case Takes::text:
        break;
    }
    return "value: ";
}

std::string zsh_spec(const Completable& option) {
    const auto description = zsh_bracketed(option.description);
    const auto value = option.takes == Takes::nothing
                           ? std::string{}
                           : ":" + zsh_quoted(zsh_action(option.takes, option.choices));
    const auto suffix = [&](const std::string& name) {
        if (option.takes == Takes::nothing) {
            return name;
        }
        return name + (name.starts_with("--") ? "=" : "+");
    };
    if (option.names.size() == 1) {
        return std::format("'{}[{}]{}'", suffix(option.names.front()), description, value);
    }
    std::vector<std::string> names;
    names.reserve(option.names.size());
    for (const auto& name : option.names) {
        names.push_back(suffix(name));
    }
    return std::format("'({})'{{{}}}'[{}]{}'", joined(option.names, " "), joined(names, ","),
                       description, value);
}

std::string zsh_script(const Completions& completions) {
    std::string out = "#compdef egraph\n# zsh completion for egraph, generated from its command "
                      "line.\n\n";
    out +=
        R"zsh(# Package arguments, which egraph completes from its stores; the arguments say what they are.
# _arguments puts compadd's options before the last argument, installed, atoms or repositories.
_egraph_complete() {
    local -a found categories options
    local ret=1
    [[ ${@[-1]} == atoms ]] || options=(--${@[-1]})
    # Unquoted: a leading = must be typed as \= in zsh, which expands =word to a command's path.
    found=(${(f)"$(egraph complete $options -- "${(Q)PREFIX}${(Q)SUFFIX}" 2>/dev/null)"})
    categories=(${(M)found:#*/})
    found=(${found:#*/})
    compadd "${@[1,-2]}" -S '' -a categories && ret=0
    compadd "${@[1,-2]}" -a found && ret=0
    return ret
}

_egraph() {
    local curcontext=$curcontext state line ret=1
    local -A opt_args
    _arguments -C \
)zsh";
    for (const auto& option : completions.options) {
        out += std::format("        {} \\\n", zsh_spec(option));
    }
    out += R"(        '1: :->command' \
        '*:: :->argument' && ret=0
    case $state in
    command)
        local -a commands=(
)";
    for (const auto& command : completions.commands) {
        out += std::format("            '{}:{}'\n", command.name, zsh_quoted(command.description));
    }
    out += R"(        )
        _describe -t commands command commands && ret=0
        ;;
    argument)
        curcontext=${curcontext%:*:*}:egraph-$line[1]:
        case $line[1] in
)";
    for (const auto& command : completions.commands) {
        out += std::format("        {})\n            _arguments", command.name);
        for (const auto& option : command.options) {
            out += std::format(" \\\n                {}", zsh_spec(option));
        }
        if (command.arguments != Takes::nothing) {
            out += std::format(" \\\n                '*:{}'",
                               zsh_quoted(zsh_action(command.arguments, {})));
        }
        out += " && ret=0\n            ;;\n";
    }
    out += "        esac\n        ;;\n    esac\n    return ret\n}\n\n_egraph \"$@\"\n";
    return out;
}

std::string fish_quoted(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\\' || c == '\'') {
            out += '\\';
        }
        out += c;
    }
    return out + "'";
}

// complete's flags for what a value takes.
std::string fish_values(Takes takes, const std::vector<std::string>& choices) {
    switch (takes) {
    case Takes::directory:
        return "-x -a '(__fish_complete_directories (commandline -ct))'";
    case Takes::file:
        return "-r -F";
    case Takes::command:
        return "-x -a '(__fish_complete_command)'";
    case Takes::package:
    case Takes::atom:
    case Takes::repository:
        return std::format("-x -a '(__egraph_complete{})'", complete_option(takes));
    case Takes::choice:
        return std::format("-x -a {}", fish_quoted(joined(choices, " ")));
    case Takes::nothing:
        break;
    case Takes::text:
        return "-x";
    }
    return "";
}

void fish_options(std::string& out, const std::vector<Completable>& options,
                  const std::string& condition) {
    for (const auto& option : options) {
        out += std::format("complete -c egraph -n {}", condition);
        for (const auto& name : option.names) {
            out += name.starts_with("--") ? std::format(" -l {}", name.substr(2))
                                          : std::format(" -s {}", name.substr(1));
        }
        if (const auto values = fish_values(option.takes, option.choices); !values.empty()) {
            out += " " + values;
        }
        out += std::format(" -d {}\n", fish_quoted(option.description));
    }
}

std::string fish_script(const Completions& completions) {
    std::string out = "# fish completion for egraph, generated from its command line.\n\n";
    out +=
        R"fish(# Package arguments, which egraph completes from its stores; the arguments say what they are.
function __egraph_complete
    egraph complete $argv -- (commandline -ct) 2>/dev/null
end

# The command given so far, stepping over the global options' values.
function __egraph_command
    set -l tokens (commandline -opc)
    set -e tokens[1]
    while set -q tokens[1]
        switch $tokens[1]
)fish";
    if (const auto names = valued(completions.options); !names.empty()) {
        out += std::format("            case {}\n                set -e tokens[1]\n",
                           joined(names, " "));
    }
    out += R"(            case '-*'
            case '*'
                echo $tokens[1]
                return 0
        end
        set -e tokens[1]
    end
    return 1
end

function __egraph_in
    set -l command (__egraph_command); and test "$command" = $argv[1]
end

complete -c egraph -f
)";
    fish_options(out, completions.options, "'not __egraph_command'");
    for (const auto& command : completions.commands) {
        out += std::format("complete -c egraph -n 'not __egraph_command' -a {} -d {}\n",
                           command.name, fish_quoted(command.description));
    }
    for (const auto& command : completions.commands) {
        const auto condition = std::format("'__egraph_in {}'", command.name);
        fish_options(out, command.options, condition);
        if (const auto values = fish_values(command.arguments, {});
            command.arguments != Takes::text && !values.empty()) {
            out += std::format("complete -c egraph -n {} {}\n", condition, values);
        }
    }
    return out;
}

} // namespace

Completions completions(const CLI::App& app) {
    Completions found{.options = options_of(app), .commands = {}};
    for (const auto* sub : app.get_subcommands({})) {
        // Hidden: not for people to type.
        if (sub->get_group().empty()) {
            continue;
        }
        CompletionCommand command{.name = sub->get_name(),
                                  .description = sub->get_description(),
                                  .options = options_of(*sub),
                                  .arguments = Takes::nothing};
        for (const auto* option : sub->get_options()) {
            if (!option->nonpositional()) {
                std::vector<std::string> choices;
                command.arguments = takes_of(*option, choices);
            }
        }
        found.commands.push_back(std::move(command));
    }
    return found;
}

std::string completion_script(const Completions& completions, CompletionShell shell) {
    switch (shell) {
    case CompletionShell::bash:
        return bash_script(completions);
    case CompletionShell::zsh:
        return zsh_script(completions);
    case CompletionShell::fish:
        return fish_script(completions);
    }
    return {};
}

} // namespace egraph
