#include "cli.hpp"

#include "check.hpp"
#include "freshness.hpp"
#include "json.hpp"
#include "os.hpp"
#include "store.hpp"
#include "version.hpp"

#include <CLI/CLI.hpp>

#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ostream>
#include <random>
#include <string_view>
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

// Runs egraph-build; the error when it could not run or did not succeed.
std::optional<std::string> run_builder(const Invocation& invocation, std::string_view mode,
                                       const std::filesystem::path& path) {
    const auto status = os::run(builder_command(invocation, mode, path));
    if (!status) {
        return status.error().message;
    }
    if (*status != 0) {
        return std::format("{} exited with status {}", invocation.builder, *status);
    }
    return std::nullopt;
}

// The store, refreshed through egraph-build first if its inputs changed.
std::expected<Store, std::string> open_store(const Invocation& invocation, std::ostream& err) {
    const auto path = store_path(invocation);
    auto store = load(path);
    if (store) {
        const auto reason = staleness(*store);
        if (!reason) {
            return std::move(*store);
        }
        if (invocation.no_refresh) {
            err << "egraph: warning: answering from a stale store (" << *reason << ")\n";
            return std::move(*store);
        }
    } else if (invocation.no_refresh) {
        return std::unexpected(store.error().message);
    }
    if (auto error = run_builder(invocation, "--incremental", path)) {
        return std::unexpected(std::move(*error));
    }
    return load(path).transform_error([](const StoreError& error) { return error.message; });
}

Exit execute(const std::monostate&, const Invocation&, std::ostream&, std::ostream& err) {
    err << "egraph: no command given\n";
    return Exit::usage;
}

template <class C> Exit execute(const C&, const Invocation&, std::ostream&, std::ostream& err) {
    err << "egraph: " << C::name << ": not implemented\n";
    return Exit::not_implemented;
}

Exit execute(const Rebuild&, const Invocation& invocation, std::ostream&, std::ostream& err) {
    if (const auto error = run_builder(invocation, "--full", store_path(invocation))) {
        err << "egraph: " << *error << '\n';
        return Exit::failure;
    }
    return Exit::ok;
}

// A path no other egraph check is using.
std::filesystem::path scratch_store() {
    std::random_device random;
    const auto name = std::format("egraph-check-{:08x}{:08x}.egraph", random(), random());
    return std::filesystem::temp_directory_path() / name;
}

Exit execute(const Check&, const Invocation& invocation, std::ostream& out, std::ostream& err) {
    // Deliberately not refreshed: the point is to compare what is stored.
    const auto stored = load(store_path(invocation));
    if (!stored) {
        err << "egraph: " << stored.error().message << '\n';
        return Exit::failure;
    }
    const auto path = scratch_store();
    const auto error = run_builder(invocation, "--full", path);
    const auto fresh = error ? std::expected<Store, StoreError>{} : load(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (error) {
        err << "egraph: " << *error << '\n';
        return Exit::failure;
    }
    if (!fresh) {
        err << "egraph: " << fresh.error().message << '\n';
        return Exit::failure;
    }
    const auto lines = drift(*stored, *fresh);
    for (const auto& line : lines) {
        out << line << '\n';
    }
    return lines.empty() ? Exit::ok : Exit::drift;
}

Exit execute(const Export& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    if (command.format != ExportFormat::json || !command.packages.empty()) {
        err << "egraph: export: only --format json of the whole graph is implemented\n";
        return Exit::not_implemented;
    }
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    write_json(out, *store);
    return Exit::ok;
}

} // namespace

void configure(CLI::App& app, Invocation& invocation) {
    app.description("Query the dependency graph of the installed packages");
    app.set_version_flag("--version", std::string{version});
    app.require_subcommand(1);

    app.add_option("--root", invocation.root, "Root whose installed packages to query")
        ->envname("ROOT")
        ->capture_default_str();
    app.add_option("--config-root", invocation.config_root,
                   "Root of the portage configuration to evaluate them with")
        ->envname("PORTAGE_CONFIGROOT");
    app.add_option("--eprefix", invocation.eprefix, "Offset prefix of a prefix installation")
        ->envname("PORTAGE_OVERRIDE_EPREFIX");
    app.add_option("--store", invocation.store,
                   "Store file to read (default: ${ROOT}${EPREFIX}/var/cache/egraph/"
                   "installed.egraph)")
        ->envname("EGRAPH_STORE");
    app.add_option("--builder", invocation.builder, "egraph-build command that refreshes the store")
        ->envname("EGRAPH_BUILD")
        ->capture_default_str();
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

std::filesystem::path store_path(const Invocation& invocation) {
    return invocation.store.value_or(
        default_store_path(invocation.root, invocation.eprefix.value_or("")));
}

std::vector<std::string> builder_command(const Invocation& invocation, std::string_view mode,
                                         const std::filesystem::path& path) {
    std::vector<std::string> argv{invocation.builder, std::string{mode}, "--store",
                                  path.string(),      "--root",          invocation.root.string()};
    if (invocation.config_root) {
        argv.insert(argv.end(), {"--config-root", invocation.config_root->string()});
    }
    if (invocation.eprefix) {
        argv.insert(argv.end(), {"--eprefix", invocation.eprefix->string()});
    }
    return argv;
}

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    return std::visit([&](const auto& command) { return execute(command, invocation, out, err); },
                      invocation.command);
}

} // namespace egraph
