#include "cli.hpp"
#include "os.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        CLI::App app{"", "egraph"};
        egraph::Invocation invocation;
        invocation.terminal = egraph::os::stdout_is_terminal();
        invocation.no_color = !egraph::os::environment("NO_COLOR").value_or("").empty();
        egraph::configure(app, invocation);
        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& e) {
            // --help and --version arrive here too, with exit code 0.
            const int code = app.exit(e);
            return code == 0 ? 0 : static_cast<int>(egraph::Exit::usage);
        }
        return static_cast<int>(egraph::run(invocation, std::cout, std::cerr));
    } catch (const std::exception& e) {
        std::cerr << "egraph: internal error: " << e.what() << '\n';
        return static_cast<int>(egraph::Exit::failure);
    }
}
