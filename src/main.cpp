#include "cli.hpp"
#include "os.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    try {
        CLI::App app{"", "egraph"};
        egraph::Invocation invocation;
        invocation.terminal = egraph::os::stdout_is_terminal();
        invocation.no_color = !egraph::os::environment("NO_COLOR").value_or("").empty();
        const auto colorterm = egraph::os::environment("COLORTERM").value_or("");
        invocation.truecolor = colorterm == "truecolor" || colorterm == "24bit";
        invocation.utf8 = egraph::os::utf8_locale();
        invocation.program_dir = egraph::os::executable().parent_path();
        if (const auto xdg = egraph::os::environment("XDG_CACHE_HOME");
            xdg && std::filesystem::path{*xdg}.is_absolute()) {
            invocation.cache_home = *xdg;
        } else if (const auto home = egraph::os::environment("HOME"); home && !home->empty()) {
            invocation.cache_home = std::filesystem::path{*home} / ".cache";
        }
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
