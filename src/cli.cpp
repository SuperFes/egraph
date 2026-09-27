#include "cli.hpp"

#include "version.hpp"

#include <CLI/CLI.hpp>

#include <map>
#include <ostream>
#include <utility>

namespace egraph {

namespace {

// The subcommand for C; its options write into invocation.command once C is active.
template <class C>
CLI::App* add_command(CLI::App& app, Invocation& invocation, std::string description) {
    CLI::App* sub = app.add_subcommand(std::string{C::name}, std::move(description));
    sub->preparse_callback([&invocation](std::size_t) { invocation.command.emplace<C>(); });
    return sub;
}

template <class C, class T>
CLI::Option* add_field(CLI::App* sub, Invocation& invocation, std::string option, T C::* field,
                       std::string description) {
    return sub->add_option_function<T>(
        std::move(option),
        [&invocation, field](const T& value) { std::get<C>(invocation.command).*field = value; },
        std::move(description));
}

Exit execute(const std::monostate&, const Invocation&, std::ostream&, std::ostream& err) {
    err << "egraph: no command given\n";
    return Exit::usage;
}

template <class C> Exit execute(const C&, const Invocation&, std::ostream&, std::ostream& err) {
    err << "egraph: " << C::name << ": not implemented\n";
    return Exit::not_implemented;
}

} // namespace

void configure(CLI::App& app, Invocation& invocation) {
    app.description("Query the dependency graph of the installed packages");
    app.set_version_flag("--version", std::string{version});
    app.require_subcommand(1);

    app.add_option("--store", invocation.store, "Store file to read")->envname("EGRAPH_STORE");
    app.add_flag("--no-refresh", invocation.no_refresh,
                 "Answer from a stale store instead of rebuilding it");

    add_field(add_command<Deps>(app, invocation, "What an installed package depends on"),
              invocation, "package", &Deps::package, "Package atom")
        ->required();
    add_field(add_command<Rdeps>(app, invocation, "What depends on an installed package"),
              invocation, "package", &Rdeps::package, "Package atom")
        ->required();
    add_field(add_command<Why>(app, invocation, "Path from @world or @system to a package"),
              invocation, "package", &Why::package, "Package atom")
        ->required();
    add_field(add_command<Soname>(app, invocation, "Installed consumers of a soname"), invocation,
              "soname", &Soname::soname, "Soname, such as libssl.so.3")
        ->required();
    add_command<Broken>(app, invocation, "Installed dependencies nothing installed satisfies");
    add_command<Orphans>(app, invocation, "Installed packages no root reaches");

    CLI::App* export_cmd = add_command<Export>(app, invocation, "Export part of the graph");
    const std::map<std::string, ExportFormat> formats{{"dot", ExportFormat::dot},
                                                      {"json", ExportFormat::json}};
    add_field(export_cmd, invocation, "--format", &Export::format, "Output format")
        ->transform(CLI::CheckedTransformer(formats, CLI::ignore_case).description("{dot,json}"));
    add_field(export_cmd, invocation, "packages", &Export::packages,
              "Packages whose neighborhood to export; all when omitted");

    add_command<Stats>(app, invocation, "Store and graph statistics");
    add_command<Rebuild>(app, invocation, "Rebuild the store from scratch");
    add_command<Check>(app, invocation, "Diff the store against a fresh build");
}

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    return std::visit([&](const auto& command) { return execute(command, invocation, out, err); },
                      invocation.command);
}

} // namespace egraph
