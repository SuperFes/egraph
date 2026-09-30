// Prints the completion script for the shell named first, for the build to install.

#include "cli.hpp"
#include "completion.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        CLI::App generator{"Print egraph's completion script for a shell", "egraph-completions"};
        std::string shell;
        generator.add_option("shell", shell, "bash, zsh or fish")
            ->required()
            ->check(CLI::IsMember({"bash", "zsh", "fish"}));
        CLI11_PARSE(generator, argc, argv);
        CLI::App app{"", "egraph"};
        egraph::Invocation invocation;
        egraph::configure(app, invocation);
        std::cout << egraph::completion_script(egraph::completions(app),
                                               shell == "bash"  ? egraph::CompletionShell::bash
                                               : shell == "zsh" ? egraph::CompletionShell::zsh
                                                                : egraph::CompletionShell::fish);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "egraph-completions: " << e.what() << '\n';
        return 1;
    }
}
