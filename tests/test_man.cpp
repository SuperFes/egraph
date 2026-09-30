#include "cli.hpp"
#include "completion.hpp"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_page(const std::string& name) {
    std::ifstream in{std::string{EGRAPH_SOURCE_DIR} + "/docs/" + name};
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// A name as roff spells it, every hyphen escaped.
std::string roff(const std::string& name) {
    std::string out;
    for (const char c : name) {
        out += c == '-' ? std::string{"\\-"} : std::string(1, c);
    }
    return out;
}

// The page from its line starting with heading up to the next section or subsection.
std::string section(const std::string& page, const std::string& heading) {
    const auto start = page.find("\n" + heading);
    if (start == std::string::npos) {
        return {};
    }
    const auto next_section = page.find("\n.SH", start + 1);
    const auto next_sub = page.find("\n.SS", start + 1);
    return page.substr(start, std::min(next_section, next_sub) - start);
}

} // namespace

TEST_CASE("the man page documents every command and option, each where it belongs") {
    CLI::App app{"", "egraph"};
    egraph::Invocation invocation;
    egraph::configure(app, invocation);
    const auto found = egraph::completions(app);
    const auto page = read_page("egraph.1");

    const auto global = section(page, ".SH OPTIONS");
    REQUIRE_FALSE(global.empty());
    for (const auto& option : found.options) {
        for (const auto& name : option.names) {
            INFO(name);
            CHECK(global.contains(roff(name)));
        }
    }
    std::vector<std::string> documented;
    for (const auto& command : found.commands) {
        INFO(command.name);
        // A subsection's heading is the command, then its arguments.
        const auto own = section(page, ".SS " + command.name + "\n").empty()
                             ? section(page, ".SS " + command.name + " ")
                             : section(page, ".SS " + command.name + "\n");
        REQUIRE_FALSE(own.empty());
        for (const auto& option : command.options) {
            for (const auto& name : option.names) {
                if (name == "-h" || name == "--help") {
                    continue;
                }
                INFO(name);
                CHECK(own.contains(roff(name)));
            }
        }
    }
    // And nothing it documents as a command is not one.
    std::istringstream lines{page};
    for (std::string line; std::getline(lines, line);) {
        if (line.starts_with(".SS ")) {
            const auto name = line.substr(4, line.find(' ', 4) - 4);
            INFO(name);
            CHECK(std::ranges::contains(found.commands, name, &egraph::CompletionCommand::name));
        }
    }
}
