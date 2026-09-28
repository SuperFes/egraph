#include "cli.hpp"

#include "atom.hpp"
#include "build_info.hpp"
#include "check.hpp"
#include "depclean.hpp"
#include "freshness.hpp"
#include "graph.hpp"
#include "human.hpp"
#include "json.hpp"
#include "os.hpp"
#include "store.hpp"

#include <CLI/CLI.hpp>

#include <expected>
#include <format>
#include <map>
#include <numeric>
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

struct Output {
    bool human;
    Painter paint;
};

Output output(const Invocation& invocation) {
    const auto chosen = style(invocation);
    return {.human = chosen.human, .paint = Painter{chosen.color}};
}

void write_lines(std::ostream& out, std::span<const std::string> lines) {
    for (const auto& line : lines) {
        out << line << '\n';
    }
}

std::vector<std::string> cpvs(const Store& store, std::span<const std::uint32_t> ids) {
    std::vector<std::string> found;
    found.reserve(ids.size());
    for (const auto id : ids) {
        found.emplace_back(store.string(store.packages.at(id).cpv));
    }
    return found;
}

// Package ids for every argument, or nothing after reporting the first that names none.
std::optional<std::vector<std::uint32_t>>
resolve_all(const Store& store, const std::vector<std::string>& arguments, std::ostream& err) {
    std::vector<std::uint32_t> ids;
    for (const auto& argument : arguments) {
        const auto found = resolve(store, argument);
        if (!found) {
            err << "egraph: " << found.error() << '\n';
            return std::nullopt;
        }
        if (found->empty()) {
            err << "egraph: " << argument << ": no installed package matches\n";
            return std::nullopt;
        }
        ids.insert(ids.end(), found->begin(), found->end());
    }
    std::ranges::sort(ids);
    const auto duplicates = std::ranges::unique(ids);
    ids.erase(duplicates.begin(), duplicates.end());
    return ids;
}

Exit edges(const std::vector<std::string>& packages, bool reverse, const Invocation& invocation,
           std::ostream& out, std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    const auto ids = resolve_all(*store, packages, err);
    if (!ids) {
        return Exit::failure;
    }
    const auto graph = build_graph(*store);
    std::vector<Edge> found;
    for (const auto id : *ids) {
        const auto some = reverse ? graph.rdeps(id) : graph.deps(id);
        found.insert(found.end(), some.begin(), some.end());
    }
    const auto lines = edge_lines(*store, found);
    if (const auto style = output(invocation); style.human) {
        human_edges(out, lines, cpvs(*store, *ids), reverse, style.paint);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Deps& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    return edges(command.packages, false, invocation, out, err);
}

Exit execute(const Rdeps& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    return edges(command.packages, true, invocation, out, err);
}

Exit execute(const Match& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    std::vector<Atom> atoms;
    for (const auto& text : command.atoms) {
        auto atom = parse_atom(text);
        if (!atom) {
            err << "egraph: " << atom.error() << '\n';
            return Exit::failure;
        }
        atoms.push_back(std::move(*atom));
    }
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        for (const auto& pkg : store->packages) {
            if (matches(*store, pkg, atoms.at(i))) {
                lines.push_back(std::format("{}\t{}", command.atoms.at(i), store->string(pkg.cpv)));
            }
        }
    }
    if (const auto style = output(invocation); style.human) {
        human_match(out, lines, command.atoms, style.paint);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Soname& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    const auto lines = soname_users(*store, command.soname, command.providers);
    if (const auto style = output(invocation); style.human) {
        human_soname(out, lines, command.soname, command.providers, style.paint);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Broken&, const Invocation& invocation, std::ostream& out, std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    const auto lines = broken(*store);
    if (const auto style = output(invocation); style.human) {
        human_broken(out, lines, style.paint);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Orphans& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    // Everything would be an orphan; depclean refuses, and so do we.
    if (store->roots.empty()) {
        err << "egraph: orphans: the @world set is empty\n";
        return Exit::failure;
    }
    const auto kept = keep(*store, {.build_deps = command.build_deps});
    const auto lines = cpvs(*store, orphans(kept));
    if (const auto style = output(invocation); style.human) {
        human_orphans(out, lines, style.paint);
    } else {
        write_lines(out, lines);
    }
    const auto unresolved = unresolved_lines(*store, kept);
    if (unresolved.empty()) {
        return Exit::ok;
    }
    err << "egraph: orphans: depclean would refuse to run; nothing installed satisfies these "
           "runtime dependencies:\n";
    for (const auto& line : unresolved) {
        err << "  " << line << '\n';
    }
    return Exit::failure;
}

Exit execute(const Why& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    const auto ids = resolve_all(*store, {command.package}, err);
    if (!ids) {
        return Exit::failure;
    }
    const auto kept = keep(*store, {.build_deps = command.build_deps});
    auto exit = Exit::ok;
    bool first = true;
    for (const auto id : *ids) {
        const auto path = why(kept, id);
        if (!path) {
            err << "egraph: why: " << store->string(store->packages.at(id).cpv)
                << ": nothing keeps it; depclean would remove it\n";
            exit = Exit::failure;
            continue;
        }
        out << (first ? "" : "\n");
        first = false;
        const auto lines = path_lines(*store, *path);
        if (const auto style = output(invocation); style.human) {
            human_path(out, lines, style.paint);
        } else {
            write_lines(out, lines);
        }
    }
    return exit;
}

Exit execute(const Stats&, const Invocation& invocation, std::ostream& out, std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    write_stats(out, *store, build_graph(*store), store_path(invocation));
    return Exit::ok;
}

Exit execute(const Export& command, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto store = open_store(invocation, err);
    if (!store) {
        err << "egraph: " << store.error() << '\n';
        return Exit::failure;
    }
    const auto graph = build_graph(*store);
    std::vector<std::uint32_t> roots;
    std::vector<std::uint32_t> packages;
    if (command.packages.empty()) {
        packages.resize(store->packages.size());
        std::ranges::iota(packages, 0U);
    } else {
        auto ids = resolve_all(*store, command.packages, err);
        if (!ids) {
            return Exit::failure;
        }
        roots = std::move(*ids);
        packages = neighborhood(graph, roots, command.depth, command.direction);
    }
    if (command.format == ExportFormat::json) {
        write_json(out, *store, packages);
    } else {
        write_dot(out, *store, graph, packages, roots);
    }
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
    const std::map<std::string, Layout> layouts{
        {"auto", Layout::automatic}, {"human", Layout::human}, {"lines", Layout::lines}};
    app.add_option("--layout", invocation.layout,
                   "Results for people, or as tab-separated lines for scripts (default auto: "
                   "for people on a terminal)")
        ->transform(CLI::CheckedTransformer(layouts).description("{auto,human,lines}"))
        ->envname("EGRAPH_LAYOUT");
    const std::map<std::string, ColorMode> colors{
        {"auto", ColorMode::automatic}, {"always", ColorMode::always}, {"never", ColorMode::never}};
    app.add_option("--color", invocation.color,
                   "Colour the human layout (default auto: on a terminal, unless NO_COLOR is set)")
        ->transform(CLI::CheckedTransformer(colors).description("{auto,always,never}"));

    add_field(add_command<Deps>(app, invocation, "What installed packages depend on"), invocation,
              "packages", &Deps::packages, "Installed cpvs, or cps for every installed version")
        ->required();
    add_field(add_command<Rdeps>(app, invocation, "What depends on installed packages"), invocation,
              "packages", &Rdeps::packages, "Installed cpvs, or cps for every installed version")
        ->required();
    const std::map<std::string, bool> yes_no{{"y", true}, {"n", false}};
    CLI::App* why_cmd = add_command<Why>(
        app, invocation, "Shortest chain of dependencies from a root set that keeps a package");
    add_field(why_cmd, invocation, "package", &Why::package,
              "Portage atom; every installed package it matches is explained")
        ->required();
    add_field(why_cmd, invocation, "--with-bdeps", &Why::build_deps,
              "Whether build-time dependencies keep packages, as emerge's option (default y)")
        ->transform(CLI::CheckedTransformer(yes_no).description("{y,n}"));
    add_field(add_command<Match>(app, invocation, "Installed packages each atom matches"),
              invocation, "atoms", &Match::atoms, "Portage atoms, such as '>=dev-libs/openssl-3:0'")
        ->required();
    CLI::App* soname = add_command<Soname>(app, invocation, "Installed consumers of a soname");
    add_field(soname, invocation, "soname", &Soname::soname, "Soname, such as libssl.so.3")
        ->required();
    soname->add_flag_callback(
        "--providers", [&invocation] { std::get<Soname>(invocation.command).providers = true; },
        "List the packages providing it instead");
    add_command<Broken>(app, invocation, "Installed dependencies nothing installed satisfies");
    CLI::App* orphans_cmd =
        add_command<Orphans>(app, invocation, "Installed packages emerge --depclean would remove");
    add_field(orphans_cmd, invocation, "--with-bdeps", &Orphans::build_deps,
              "Whether build-time dependencies keep packages, as emerge's option (default y)")
        ->transform(CLI::CheckedTransformer(yes_no).description("{y,n}"));

    CLI::App* export_cmd = add_command<Export>(app, invocation, "Export part of the graph");
    const std::map<std::string, ExportFormat> formats{{"dot", ExportFormat::dot},
                                                      {"json", ExportFormat::json}};
    add_field(export_cmd, invocation, "--format", &Export::format, "Output format")
        ->transform(CLI::CheckedTransformer(formats, CLI::ignore_case).description("{dot,json}"));
    add_field(export_cmd, invocation, "packages", &Export::packages,
              "Packages whose neighborhood to export; all when omitted");
    add_field(export_cmd, invocation, "--depth", &Export::depth,
              "Dependency edges to follow out from the packages (default 1)");
    const std::map<std::string, Direction> directions{{"reverse", Direction::reverse},
                                                      {"forward", Direction::forward},
                                                      {"both", Direction::both}};
    add_field(export_cmd, invocation, "--direction", &Export::direction,
              "Follow reverse dependencies, forward ones, or both (default reverse)")
        ->transform(CLI::CheckedTransformer(directions).description("{reverse,forward,both}"));

    add_command<Stats>(app, invocation, "Store and graph statistics");
    add_command<Rebuild>(app, invocation, "Rebuild the store from scratch");
    add_command<Check>(app, invocation, "Diff the store against a fresh build");
}

Style style(const Invocation& invocation) {
    const bool human = invocation.layout == Layout::human ||
                       (invocation.layout == Layout::automatic && invocation.terminal);
    const bool color =
        human &&
        (invocation.color == ColorMode::always ||
         (invocation.color == ColorMode::automatic && invocation.terminal && !invocation.no_color));
    return {.human = human, .color = color};
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
