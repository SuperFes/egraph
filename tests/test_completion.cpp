#include "completion.hpp"

#include "cli.hpp"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using egraph::CompletionShell;
using egraph::Takes;

namespace {

egraph::Completions egraph_completions() {
    CLI::App app{"", "egraph"};
    egraph::Invocation invocation;
    egraph::configure(app, invocation);
    return egraph::completions(app);
}

const egraph::Completable* option(const std::vector<egraph::Completable>& options,
                                  const std::string& name) {
    const auto found = std::ranges::find_if(options, [&](const auto& candidate) {
        return std::ranges::contains(candidate.names, name);
    });
    return found == options.end() ? nullptr : &*found;
}

const egraph::CompletionCommand* command(const egraph::Completions& completions,
                                         const std::string& name) {
    const auto found =
        std::ranges::find(completions.commands, name, &egraph::CompletionCommand::name);
    return found == completions.commands.end() ? nullptr : &*found;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("completions follow the command line's options, commands and what each takes") {
    const auto found = egraph_completions();

    const auto* root = option(found.options, "--root");
    REQUIRE(root != nullptr);
    CHECK(root->takes == Takes::directory);
    CHECK(root->description == "Root whose installed packages to query");
    CHECK(option(found.options, "--store")->takes == Takes::file);
    CHECK(option(found.options, "--builder")->takes == Takes::command);
    CHECK(option(found.options, "--no-refresh")->takes == Takes::nothing);
    const auto* layout = option(found.options, "--layout");
    REQUIRE(layout != nullptr);
    CHECK(layout->takes == Takes::choice);
    CHECK(layout->choices == std::vector<std::string>{"auto", "human", "lines"});
    const auto* help = option(found.options, "--help");
    REQUIRE(help != nullptr);
    CHECK(help->names == std::vector<std::string>{"-h", "--help"});
    CHECK(option(found.options, "--version") != nullptr);
    // Positional arguments belong to commands, not to the options.
    CHECK(std::ranges::none_of(found.options, [](const auto& each) { return each.names.empty(); }));

    REQUIRE(found.commands.size() == 21);
    CHECK(found.commands.front().name == "deps");
    const auto* updates = command(found, "updates");
    REQUIRE(updates != nullptr);
    CHECK(updates->description == "Installed packages emerge -u would replace or rebuild");
    CHECK(updates->arguments == Takes::nothing);
    const auto* table = option(updates->options, "--table");
    REQUIRE(table != nullptr);
    CHECK(table->names == std::vector<std::string>{"-t", "--table"});
    CHECK(table->takes == Takes::nothing);
    CHECK(option(updates->options, "--dynamic-deps")->choices ==
          std::vector<std::string>{"y", "n"});
    CHECK(option(updates->options, "--root") == nullptr);

    CHECK(command(found, "why")->arguments == Takes::package);
    CHECK(command(found, "rdeps")->arguments == Takes::package);
    CHECK(command(found, "blockers")->arguments == Takes::package);
    CHECK(command(found, "soname")->arguments == Takes::text);
    CHECK(command(found, "stats")->arguments == Takes::nothing);
    CHECK(command(found, "install")->arguments == Takes::package);
    CHECK(option(command(found, "update")->options, "--yes")->names ==
          std::vector<std::string>{"-y", "--yes"});
    CHECK(option(command(found, "export")->options, "--format")->choices ==
          std::vector<std::string>{"dot", "json"});
    CHECK(option(command(found, "affected")->options, "--request")->takes == Takes::file);
}

TEST_CASE("each shell's script names every command and option") {
    const auto found = egraph_completions();
    for (const auto shell : {CompletionShell::bash, CompletionShell::zsh, CompletionShell::fish}) {
        const auto script = egraph::completion_script(found, shell);
        INFO(static_cast<int>(shell));
        for (const auto& each : found.commands) {
            CHECK(contains(script, each.name));
            for (const auto& opt : each.options) {
                for (const auto& name : opt.names) {
                    const auto spelled = name.starts_with("--")           ? name.substr(2)
                                         : shell == CompletionShell::fish ? "-s " + name.substr(1)
                                                                          : name;
                    CHECK(contains(script, spelled));
                }
            }
        }
        CHECK(contains(script, "var/db/pkg"));
    }
    CHECK(egraph::completion_script(found, CompletionShell::bash)
              .ends_with("complete -F _egraph egraph\n"));
    CHECK(egraph::completion_script(found, CompletionShell::zsh).starts_with("#compdef egraph\n"));
    CHECK(contains(egraph::completion_script(found, CompletionShell::fish),
                   "complete -c egraph -n 'not __egraph_command' -a updates -d 'Installed "
                   "packages emerge -u would replace or rebuild'"));
}

TEST_CASE("descriptions are quoted for each shell") {
    CLI::App app{"", "egraph"};
    app.add_flag("--odd", "it's [odd]: \\ yes");
    const auto found = egraph::completions(app);
    CHECK(contains(egraph::completion_script(found, CompletionShell::zsh),
                   R"('--odd[it'\''s \[odd\]: \\ yes]')"));
    CHECK(contains(egraph::completion_script(found, CompletionShell::fish),
                   R"(-l odd -d 'it\'s [odd]: \\ yes')"));
}
