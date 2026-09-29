#include "cli.hpp"

#include "affected.hpp"

#include "atom.hpp"
#include "build_info.hpp"
#include "check.hpp"
#include "depclean.hpp"
#include "emerge.hpp"
#include "evaluated.hpp"
#include "freshness.hpp"
#include "graph.hpp"
#include "human.hpp"
#include "json.hpp"
#include "os.hpp"
#include "pressure.hpp"
#include "session.hpp"
#include "steve.hpp"
#include "store.hpp"
#include "tui.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
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

// Converts an option's value from one of the names, which the help shows in this order; anything
// else, the values' own spellings included, fails naming them.
template <class T>
CLI::Validator one_of(std::vector<std::pair<std::string, T>> choices, bool ignore_case = false) {
    std::string names;
    std::string listed;
    for (const auto& [name, value] : choices) {
        names += (names.empty() ? "" : ",") + name;
        listed += (listed.empty() ? "" : ", ") + name;
    }
    const auto folded = [ignore_case](std::string text) {
        if (ignore_case) {
            std::ranges::transform(text, text.begin(), [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
        }
        return text;
    };
    return {[choices = std::move(choices), listed, folded](std::string& input) -> std::string {
                const auto wanted = folded(input);
                const auto found =
                    std::ranges::find(choices, wanted, &std::pair<std::string, T>::first);
                if (found == choices.end()) {
                    return std::format("{} is not one of {}", input, listed);
                }
                // What CLI11 converts back to the option's type.
                if constexpr (std::is_enum_v<T>) {
                    input = std::to_string(std::to_underlying(found->second));
                } else {
                    input = found->second ? "true" : "false";
                }
                return {};
            },
            std::format("{{{}}}", names)};
}

Exit execute(const std::monostate&, Session&, const Invocation&, std::ostream&, std::ostream& err) {
    err << "egraph: no command given\n";
    return Exit::usage;
}

Exit execute(const Rebuild&, Session&, const Invocation& invocation, std::ostream&,
             std::ostream& err) {
    if (const auto error = run_builder(invocation, "--full", store_path(invocation))) {
        err << "egraph: " << *error << '\n';
        return Exit::failure;
    }
    return Exit::ok;
}

Exit execute(const Refresh&, Session& session, const Invocation&, std::ostream&,
             std::ostream& err) {
    if (const auto stores = session.stores(); !stores) {
        err << "egraph: " << stores.error() << '\n';
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

// The last count lines of a file, joined; empty when it cannot be read.
std::string last_lines(const std::filesystem::path& path, std::size_t count) {
    std::ifstream in{path};
    std::deque<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(std::move(line));
        if (lines.size() > count) {
            lines.pop_front();
        }
    }
    std::string joined;
    for (const auto& line : lines) {
        joined += (joined.empty() ? "" : "\n") + line;
    }
    return joined;
}

// What argv prints, stdout and stderr together, or why it failed: its output, or its status.
std::expected<std::string, std::string> output_of(const std::vector<std::string>& argv) {
    const auto log = std::filesystem::path{scratch_store()}.replace_extension(".out");
    const auto status = os::run(argv, log);
    std::ifstream in{log};
    std::ostringstream text;
    text << in.rdbuf();
    in.close();
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    auto output = text.str();
    while (output.ends_with('\n')) {
        output.pop_back();
    }
    if (!status) {
        return std::unexpected(status.error().message);
    }
    if (*status != 0) {
        return std::unexpected(output.empty()
                                   ? std::format("{} exited with status {}", argv.front(), *status)
                                   : output);
    }
    return output;
}

// The same root, however it is spelled: "/mnt/gentoo" and "/mnt/gentoo/".
bool same_root(const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto normal = [](const std::filesystem::path& path) {
        auto text = path.lexically_normal().string();
        while (text.size() > 1 && text.ends_with('/')) {
            text.pop_back();
        }
        return text;
    };
    return normal(a) == normal(b);
}

// The running emerge's merge list, for this root.
std::vector<emerge::Pending> read_merge_list(const Invocation& invocation) {
    std::ifstream in{emerge::mtimedb_path(invocation.eprefix.value_or(""))};
    std::ostringstream text;
    text << in.rdbuf();
    auto list = emerge::parse_mergelist(text.str());
    std::erase_if(list, [&](const emerge::Pending& pending) {
        return !same_root(pending.root, invocation.root);
    });
    return list;
}

// What each merge list package waits for, through egraph-build --pending; an error is its last
// line of output.
std::expected<emerge::Waits, std::string> plan(const Invocation& invocation,
                                               const std::vector<emerge::Pending>& list) {
    std::vector<std::string> entries;
    entries.reserve(list.size());
    for (const auto& pending : list) {
        entries.push_back(std::format("{}:{}", pending.kind, pending.cpv));
    }
    const auto output = std::filesystem::path{scratch_store()}.replace_extension(".json");
    const auto ran = output_of(pending_command(invocation, output, entries));
    std::ifstream in{output};
    std::ostringstream text;
    text << in.rdbuf();
    in.close();
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    if (!ran) {
        const auto& error = ran.error();
        const auto last = error.rfind('\n');
        return std::unexpected(last == std::string::npos ? error : error.substr(last + 1));
    }
    return emerge::parse_waits(text.str());
}

// The running steve as stevie reads it, or else as its command line started it.
std::optional<steve::Status> read_steve() {
    const auto command_line = steve::find_command_line();
    if (!command_line) {
        return std::nullopt;
    }
    std::string problem;
    if (const auto output = output_of(steve::get_arguments())) {
        if (auto settings = steve::parse_get(*output)) {
            return steve::Status{.live = true, .settings = *settings, .problem = {}};
        }
        problem = "stevie printed something unexpected";
    } else {
        problem = output.error();
    }
    return steve::Status{.live = false,
                         .settings = steve::parse_command_line(*command_line).value_or({}),
                         .problem = problem};
}

// A full build into path whose output goes to a log rather than the terminal; an error ends with
// the log's last lines.
std::optional<std::string> quiet_build(const Invocation& invocation,
                                       const std::filesystem::path& path) {
    const auto log = std::filesystem::path{scratch_store()}.replace_extension(".log");
    auto error = run_builder(invocation, "--full", path, log);
    const auto output = last_lines(log, 8);
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    if (error && !output.empty()) {
        *error += ":\n" + output;
    }
    return error;
}

template <class Loaded> struct ScratchBuild {
    Loaded loaded;
    ScratchStores files;
};

// A full build into scratch files, loaded by load_path (Loaded is a Store or Stores); quiet keeps
// the builder off the terminal.
template <class Loaded, class Load>
std::expected<ScratchBuild<Loaded>, std::string> scratch_build(const Invocation& invocation,
                                                               bool quiet, const Load& load_path) {
    ScratchStores files{scratch_store()};
    if (auto error = quiet ? quiet_build(invocation, files.path())
                           : run_builder(invocation, "--full", files.path())) {
        return std::unexpected(std::move(*error));
    }
    auto loaded = load_path(files.path());
    if (!loaded) {
        return std::unexpected(std::move(loaded.error().message));
    }
    return ScratchBuild<Loaded>{.loaded = std::move(*loaded), .files = std::move(files)};
}

// As scratch_build, the files removed once loaded.
template <class Loaded, class Load>
std::expected<Loaded, std::string> fresh_build(const Invocation& invocation, bool quiet,
                                               const Load& load_path) {
    return scratch_build<Loaded>(invocation, quiet, load_path)
        .transform([](ScratchBuild<Loaded>&& built) { return std::move(built).loaded; });
}

} // namespace

ScratchStores::ScratchStores(ScratchStores&& other) noexcept
    : path_{std::exchange(other.path_, {})} {}

ScratchStores& ScratchStores::operator=(ScratchStores&& other) noexcept {
    if (this != &other) {
        remove();
        path_ = std::exchange(other.path_, {});
    }
    return *this;
}

ScratchStores::~ScratchStores() {
    remove();
}

void ScratchStores::remove() const {
    if (path_.empty()) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(evaluated_store_path(path_), ignored);
}

std::expected<Stores, std::string> save_stores(const Invocation& invocation,
                                               const std::optional<ScratchStores>& checked) {
    const auto path = store_path(invocation);
    if (checked) {
        if (auto stores = load_stores(checked->path()); stores && !staleness(*stores)) {
            // Installed first, as the builder writes them.
            const std::array copies{
                std::pair{checked->path(), path},
                std::pair{evaluated_store_path(checked->path()), evaluated_store_path(path)}};
            for (const auto& [from, to] : copies) {
                if (const auto copied = os::replace_with_copy(from, to); !copied) {
                    return std::unexpected(
                        std::format("{}: {}", to.string(), copied.error().message()));
                }
            }
            return std::move(*stores);
        }
    }
    if (auto error = quiet_build(invocation, path)) {
        return std::unexpected(std::move(*error));
    }
    return load_stores(path).transform_error([](const StoreError& e) { return e.message; });
}

namespace {

Exit execute(const Check&, Session&, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    // Deliberately not refreshed: the point is to compare what queries would read.
    const auto system = system_store_path(invocation);
    const auto system_store = load(system);
    const bool current = system_store && !staleness(*system_store);
    const auto stored = load(!invocation.store && current ? system : store_path(invocation));
    if (!stored) {
        err << "egraph: " << stored.error().message << '\n';
        return Exit::failure;
    }
    const auto built =
        fresh_build<Store>(invocation, false, [](const auto& path) { return load(path); });
    if (!built) {
        err << "egraph: " << built.error() << '\n';
        return Exit::failure;
    }
    const auto lines = drift(*stored, *built);
    for (const auto& line : lines) {
        out << line << '\n';
    }
    return lines.empty() ? Exit::ok : Exit::drift;
}

struct Output {
    bool human;
    Theme theme;
};

Output output(const Invocation& invocation) {
    const auto chosen = style(invocation);
    return {.human = chosen.human,
            .theme = {.paint = Painter{chosen.color}, .glyph_set = chosen.glyphs}};
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

// Reports a session's error.
Exit fail(std::ostream& err, std::string_view message) {
    err << "egraph: " << message << '\n';
    return Exit::failure;
}

Exit edges(const std::vector<std::string>& packages, bool reverse, bool possible, Session& session,
           const Invocation& invocation, std::ostream& out, std::ostream& err) {
    if (possible && !invocation.dynamic_deps) {
        err << "egraph: --possible reads the ebuilds' dependencies, which --dynamic-deps n "
               "leaves out\n";
        return Exit::usage;
    }
    const auto loaded = session.dependencies(invocation.dynamic_deps);
    if (!loaded) {
        return fail(err, loaded.error());
    }
    const auto graph = session.graph(invocation.dynamic_deps);
    if (!graph) {
        return fail(err, graph.error());
    }
    const Store& store = *loaded;
    const auto ids = resolve_all(store, packages, err);
    if (!ids) {
        return Exit::failure;
    }
    std::vector<Edge> found;
    for (const auto id : *ids) {
        const auto some = reverse ? graph->get().rdeps(id) : graph->get().deps(id);
        found.insert(found.end(), some.begin(), some.end());
    }
    auto lines = edge_lines(store, found);
    // Only --possible needs the evaluated store past the merge.
    if (possible) {
        const auto stores = session.stores();
        if (!stores) {
            return fail(err, stores.error());
        }
        std::ranges::move(possible_lines(stores->get().evaluated, *ids, reverse),
                          std::back_inserter(lines));
        std::ranges::sort(lines);
    }
    if (const auto style = output(invocation); style.human) {
        human_edges(out, lines, cpvs(store, *ids), reverse, style.theme);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Deps& command, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    return edges(command.packages, false, command.possible, session, invocation, out, err);
}

Exit execute(const Rdeps& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    return edges(command.packages, true, command.possible, session, invocation, out, err);
}

Exit execute(const Match& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
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
    if (command.candidates) {
        const auto stores = session.stores();
        if (!stores) {
            return fail(err, stores.error());
        }
        const auto& [installed, evaluated] = stores->get();
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            for (const auto& candidate : evaluated.candidates) {
                if (matches(installed, evaluated, candidate, atoms.at(i))) {
                    lines.push_back(std::format("{}\t{}::{}", command.atoms.at(i),
                                                evaluated.string(candidate.cpv),
                                                evaluated.string(candidate.repo)));
                }
            }
        }
    } else {
        const auto loaded = session.installed();
        if (!loaded) {
            return fail(err, loaded.error());
        }
        const Store& store = *loaded;
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            for (const auto& pkg : store.packages) {
                if (matches(store, pkg, atoms.at(i))) {
                    lines.push_back(
                        std::format("{}\t{}", command.atoms.at(i), store.string(pkg.cpv)));
                }
            }
        }
    }
    if (const auto style = output(invocation); style.human) {
        human_match(out, lines, command.atoms, style.theme);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Soname& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto store = session.installed();
    if (!store) {
        return fail(err, store.error());
    }
    const auto lines = soname_users(*store, command.soname, command.providers);
    if (const auto style = output(invocation); style.human) {
        human_soname(out, lines, command.soname, command.providers, style.theme);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Broken&, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto store = session.dependencies(invocation.dynamic_deps);
    if (!store) {
        return fail(err, store.error());
    }
    if (const auto style = output(invocation); style.human) {
        const auto records = broken_records(*store);
        human_broken(out, records.broken, records.replaced, style.theme);
    } else {
        write_lines(out, broken(*store));
    }
    return Exit::ok;
}

Exit execute(const Orphans& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto depclean = session.depclean(command.build_deps, invocation.dynamic_deps);
    if (!depclean) {
        return fail(err, depclean.error());
    }
    const Store& store = depclean->get().store;
    // Everything would be an orphan; depclean refuses, and so do we.
    if (store.roots.empty()) {
        err << "egraph: orphans: the @world set is empty\n";
        return Exit::failure;
    }
    const auto& kept = depclean->get().kept;
    const auto lines = cpvs(store, orphans(kept));
    if (const auto style = output(invocation); style.human) {
        human_orphans(out, lines, style.theme);
    } else {
        write_lines(out, lines);
    }
    const auto unresolved = unresolved_lines(store, kept);
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

Exit execute(const Updates& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    // Both stores first, so that the dependencies are read from the same build.
    const auto stores = session.stores();
    if (!stores) {
        return fail(err, stores.error());
    }
    const auto store = session.dependencies(invocation.dynamic_deps);
    const auto graph = session.graph(invocation.dynamic_deps);
    if (!store || !graph) {
        return fail(err, store ? graph.error() : store.error());
    }
    const auto lines =
        update_lines(*store, *graph, stores->get().evaluated, command.rebuilds, command.held);
    if (const auto style = output(invocation); style.human) {
        human_updates(out, lines, style.theme);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

Exit execute(const Why& command, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto depclean = session.depclean(command.build_deps, invocation.dynamic_deps);
    if (!depclean) {
        return fail(err, depclean.error());
    }
    const Store& store = depclean->get().store;
    const auto ids = resolve_all(store, {command.package}, err);
    if (!ids) {
        return Exit::failure;
    }
    const auto& kept = depclean->get().kept;
    auto exit = Exit::ok;
    bool first = true;
    const auto style = output(invocation);
    for (const auto id : *ids) {
        const auto path = why(kept, id);
        if (!path) {
            err << "egraph: why: " << store.string(store.packages.at(id).cpv)
                << ": nothing keeps it; depclean would remove it\n";
            exit = Exit::failure;
            continue;
        }
        out << (first ? "" : "\n");
        first = false;
        const auto lines = path_lines(store, *path);
        if (style.human) {
            human_path(out, lines, style.theme);
        } else {
            write_lines(out, lines);
        }
    }
    if (style.human && !first) {
        human_legend(out, style.theme);
    }
    return exit;
}

Exit execute(const Tui&, Session&, const Invocation& invocation, std::ostream&, std::ostream& err) {
    if (!tui::available()) {
        err << "egraph: tui: this egraph was built without Notcurses (meson -Dtui=enabled)\n";
        return Exit::not_implemented;
    }
    if (!invocation.terminal) {
        err << "egraph: tui: standard output is not a terminal\n";
        return Exit::usage;
    }
    // Warnings would vanish under the interface, so it repeats them.
    std::stringstream warned;
    auto stores = open_stores(invocation, warned);
    err << warned.str();
    if (!stores) {
        err << "egraph: " << stores.error() << '\n';
        return Exit::failure;
    }
    std::vector<std::string> warnings;
    for (std::string line; std::getline(warned, line);) {
        constexpr std::string_view prefix = "egraph: warning: ";
        warnings.push_back(line.starts_with(prefix) ? line.substr(prefix.size()) : line);
    }
    // Only root writes the store here, from the check's build while it is still fresh; anyone
    // else previews the check's build instead.
    const bool saves = os::is_root();
    std::optional<ScratchStores> checked;
    // The builder cannot share the terminal the interface owns; its output is kept for errors.
    // The drift compares installed stores, whichever dependencies the interface reads.
    const auto check = [&invocation, &checked, saves](const Store& stored) -> tui::CheckResult {
        checked.reset();
        auto built = scratch_build<Stores>(invocation, true, load_stores);
        if (!built) {
            return std::unexpected(std::move(built.error()));
        }
        if (saves) {
            checked = std::move(built->files);
        }
        auto lines = drift(stored, built->loaded.installed);
        return tui::Fresh{.store = std::move(built->loaded.installed),
                          .evaluated = std::move(built->loaded.evaluated),
                          .drift = std::move(lines)};
    };
    tui::Rebuilder rebuild;
    if (saves) {
        rebuild = [&invocation, &checked]() {
            auto saved = save_stores(invocation, checked);
            checked.reset();
            return saved;
        };
    }
    const auto run_dir = emerge::status_dir(invocation.eprefix.value_or(""));
    const auto watch = [run_dir] { return emerge::read_snapshots(run_dir); };
    const auto sample = [] { return pressure::read_sample(); };
    const auto set_steve = [](steve::Setting setting,
                              double value) -> std::expected<void, std::string> {
        return output_of(steve::set_arguments(setting, value)).transform([](const auto&) {});
    };
    return tui::open_and_run(
        std::move(*stores), invocation.dynamic_deps, style(invocation).glyphs,
        {.check = check,
         .rebuild = rebuild,
         .watch = watch,
         .sample = sample,
         .steve = read_steve,
         .set_steve = set_steve,
         .merge_list = [&invocation] { return read_merge_list(invocation); },
         .plan = [&invocation](
                     const std::vector<emerge::Pending>& list) { return plan(invocation, list); }},
        warnings, err);
}

Exit execute(const Affected& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    std::ostringstream text;
    if (command.request == "-") {
        text << std::cin.rdbuf();
    } else {
        std::ifstream in{command.request};
        if (!in) {
            err << "egraph: affected: cannot read " << command.request << '\n';
            return Exit::failure;
        }
        text << in.rdbuf();
    }
    const auto request = parse_request(text.str());
    if (!request) {
        err << "egraph: affected: " << request.error() << '\n';
        return Exit::usage;
    }
    const auto store = session.dependencies(invocation.dynamic_deps);
    if (!store) {
        return fail(err, store.error());
    }
    out << to_json(affected(*store, *request));
    return Exit::ok;
}

Exit execute(const Stats&, Session& session, const Invocation&, std::ostream& out,
             std::ostream& err) {
    const auto store = session.installed();
    const auto graph = session.graph(false);
    if (!store || !graph) {
        return fail(err, store ? graph.error() : store.error());
    }
    write_stats(out, *store, *graph, session.used());
    return Exit::ok;
}

Exit execute(const Export& command, Session& session, const Invocation&, std::ostream& out,
             std::ostream& err) {
    if (command.evaluated) {
        if (command.format != ExportFormat::json || !command.packages.empty()) {
            err << "egraph: export --evaluated writes the whole store, as JSON\n";
            return Exit::usage;
        }
        const auto stores = session.stores();
        if (!stores) {
            return fail(err, stores.error());
        }
        write_evaluated_json(out, stores->get().evaluated);
        return Exit::ok;
    }
    const auto loaded = session.installed();
    const auto graph = session.graph(false);
    if (!loaded || !graph) {
        return fail(err, loaded ? graph.error() : loaded.error());
    }
    const Store& store = *loaded;
    std::vector<std::uint32_t> roots;
    std::vector<std::uint32_t> packages;
    if (command.packages.empty()) {
        packages.resize(store.packages.size());
        std::ranges::iota(packages, 0U);
    } else {
        auto ids = resolve_all(store, command.packages, err);
        if (!ids) {
            return Exit::failure;
        }
        roots = std::move(*ids);
        packages = neighborhood(*graph, roots, command.depth, command.direction);
    }
    if (command.format == ExportFormat::json) {
        write_json(out, store, packages);
    } else {
        write_dot(out, store, *graph, packages, roots);
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
    app.add_option("--builder", invocation.builder,
                   "egraph-build command that refreshes the store (default: the one next to "
                   "egraph, else egraph-build in PATH)")
        ->envname("EGRAPH_BUILD");
    app.add_flag("--no-refresh", invocation.no_refresh,
                 "Answer from a stale store instead of rebuilding it");
    app.add_option("--layout", invocation.layout,
                   "Results for people, or as tab-separated lines for scripts (default auto: "
                   "for people on a terminal)")
        ->transform(one_of<Layout>(
            {{"auto", Layout::automatic}, {"human", Layout::human}, {"lines", Layout::lines}}))
        ->envname("EGRAPH_LAYOUT");
    app.add_option("--color", invocation.color,
                   "Colour the human layout (default auto: on a terminal, unless NO_COLOR is set)")
        ->transform(one_of<ColorMode>({{"auto", ColorMode::automatic},
                                       {"always", ColorMode::always},
                                       {"never", ColorMode::never}}));
    app.add_option("--glyphs", invocation.glyphs,
                   "Icons in the human layout: a Nerd Font's, plain Unicode, or ASCII "
                   "(default nerd in a UTF-8 locale, ascii otherwise)")
        ->transform(one_of<GlyphSet>(
            {{"nerd", GlyphSet::nerd}, {"unicode", GlyphSet::unicode}, {"ascii", GlyphSet::ascii}}))
        ->envname("EGRAPH_GLYPHS");

    const auto yes_no = one_of<bool>({{"y", true}, {"n", false}});
    const auto add_dynamic_deps = [&invocation, &yes_no](CLI::App* sub) {
        sub->add_option_function<bool>(
               "--dynamic-deps",
               [&invocation](const bool& value) { invocation.dynamic_deps = value; },
               "Read an installed package's dependencies from its ebuild when the same version is "
               "still in its repository, as emerge's option (default y)")
            ->transform(yes_no);
        return sub;
    };
    constexpr auto possible_help = "Also what the ebuilds would add with USE flags toggled, with "
                                   "the flags";
    CLI::App* deps_cmd =
        add_dynamic_deps(add_command<Deps>(app, invocation, "What installed packages depend on"));
    add_field(deps_cmd, invocation, "packages", &Deps::packages,
              "Installed cpvs, or cps for every installed version")
        ->required();
    deps_cmd->add_flag_callback(
        "--possible", [&invocation] { std::get<Deps>(invocation.command).possible = true; },
        possible_help);
    CLI::App* rdeps_cmd =
        add_dynamic_deps(add_command<Rdeps>(app, invocation, "What depends on installed packages"));
    add_field(rdeps_cmd, invocation, "packages", &Rdeps::packages,
              "Installed cpvs, or cps for every installed version")
        ->required();
    rdeps_cmd->add_flag_callback(
        "--possible", [&invocation] { std::get<Rdeps>(invocation.command).possible = true; },
        possible_help);
    CLI::App* why_cmd = add_dynamic_deps(add_command<Why>(
        app, invocation, "Shortest chain of dependencies from a root set that keeps a package"));
    add_field(why_cmd, invocation, "package", &Why::package,
              "Portage atom; every installed package it matches is explained")
        ->required();
    add_field(why_cmd, invocation, "--with-bdeps", &Why::build_deps,
              "Whether build-time dependencies keep packages, as emerge's option (default y)")
        ->transform(yes_no);
    CLI::App* match_cmd =
        add_command<Match>(app, invocation, "Installed packages each atom matches");
    add_field(match_cmd, invocation, "atoms", &Match::atoms,
              "Portage atoms, such as '>=dev-libs/openssl-3:0'")
        ->required();
    match_cmd->add_flag_callback(
        "--candidates", [&invocation] { std::get<Match>(invocation.command).candidates = true; },
        "Match the installed cps' ebuilds instead (cpv::repo), with the USE each would be built "
        "with now, masked ones included");
    CLI::App* soname = add_command<Soname>(app, invocation, "Installed consumers of a soname");
    add_field(soname, invocation, "soname", &Soname::soname, "Soname, such as libssl.so.3")
        ->required();
    soname->add_flag_callback(
        "--providers", [&invocation] { std::get<Soname>(invocation.command).providers = true; },
        "List the packages providing it instead");
    add_dynamic_deps(
        add_command<Broken>(app, invocation, "Installed dependencies nothing installed satisfies"));
    CLI::App* orphans_cmd = add_dynamic_deps(
        add_command<Orphans>(app, invocation, "Installed packages emerge --depclean would remove"));
    add_field(orphans_cmd, invocation, "--with-bdeps", &Orphans::build_deps,
              "Whether build-time dependencies keep packages, as emerge's option (default y)")
        ->transform(yes_no);

    CLI::App* updates_cmd = add_dynamic_deps(add_command<Updates>(
        app, invocation, "Installed packages emerge -uD would replace or rebuild"));
    updates_cmd->add_flag_callback(
        "--held", [&invocation] { std::get<Updates>(invocation.command).held = true; },
        "Also the updates installed dependents hold back, and which of their atoms do");
    updates_cmd->add_flag_callback(
        "-N,--newuse",
        [&invocation] { std::get<Updates>(invocation.command).rebuilds = UseRebuilds::all; },
        "Also the rebuilds emerge --newuse makes for changed USE or IUSE");
    updates_cmd->add_flag_callback(
        "-U,--changed-use",
        [&invocation] {
            auto& rebuilds = std::get<Updates>(invocation.command).rebuilds;
            // --newuse takes in --changed-use's, as in emerge.
            rebuilds = rebuilds == UseRebuilds::all ? rebuilds : UseRebuilds::changed;
        },
        "Also the rebuilds emerge --changed-use makes for changed USE");

    CLI::App* export_cmd = add_command<Export>(app, invocation, "Export part of the graph");
    add_field(export_cmd, invocation, "--format", &Export::format, "Output format")
        ->transform(
            one_of<ExportFormat>({{"dot", ExportFormat::dot}, {"json", ExportFormat::json}}, true));
    add_field(export_cmd, invocation, "packages", &Export::packages,
              "Packages whose neighborhood to export; all when omitted");
    export_cmd->add_flag_callback(
        "--evaluated", [&invocation] { std::get<Export>(invocation.command).evaluated = true; },
        "Export the evaluated store instead, whole and as JSON: the dependencies emerge reads "
        "by default, and the versions each installed package could move to");
    add_field(export_cmd, invocation, "--depth", &Export::depth,
              "Dependency edges to follow out from the packages (default 1)");
    add_field(export_cmd, invocation, "--direction", &Export::direction,
              "Follow reverse dependencies, forward ones, or both (default reverse)")
        ->transform(one_of<Direction>({{"reverse", Direction::reverse},
                                       {"forward", Direction::forward},
                                       {"both", Direction::both}}));

    add_command<Stats>(app, invocation, "Store and graph statistics");
    add_command<Tui>(app, invocation, "Browse the graph in a terminal interface");
    add_command<Rebuild>(app, invocation, "Rebuild the store from scratch");
    add_command<Refresh>(app, invocation,
                         "Bring the store up to date if its inputs changed, printing nothing");
    add_command<Check>(app, invocation, "Diff the store against a fresh build");
    add_field(
        add_dynamic_deps(add_command<Affected>(
            app, invocation,
            "What a set of merges can affect, as JSON, for portage's neighborhood completion")),
        invocation, "--request", &Affected::request,
        "JSON request file (default -: standard input)");
}

Style style(const Invocation& invocation) {
    const bool human = invocation.layout == Layout::human ||
                       (invocation.layout == Layout::automatic && invocation.terminal);
    const bool color =
        human &&
        (invocation.color == ColorMode::always ||
         (invocation.color == ColorMode::automatic && invocation.terminal && !invocation.no_color));
    const auto glyphs =
        invocation.glyphs.value_or(invocation.utf8 ? GlyphSet::nerd : GlyphSet::ascii);
    if (!color) {
        return {.human = human, .color = ColorDepth::none, .glyphs = glyphs};
    }
    return {.human = human,
            .color = invocation.truecolor ? ColorDepth::truecolor : ColorDepth::palette,
            .glyphs = glyphs};
}

std::filesystem::path system_store_path(const Invocation& invocation) {
    return default_store_path(invocation.root, invocation.eprefix.value_or(""));
}

std::optional<std::filesystem::path> user_store_path(const Invocation& invocation) {
    if (!invocation.cache_home) {
        return std::nullopt;
    }
    // One store per EROOT: installed.egraph for /, installed-mnt-gentoo.egraph for /mnt/gentoo.
    auto eroot = (invocation.root / invocation.eprefix.value_or("").relative_path())
                     .lexically_normal()
                     .string();
    while (eroot.ends_with('/')) {
        eroot.pop_back();
    }
    std::ranges::replace(eroot, '/', '-');
    return *invocation.cache_home / "egraph" / std::format("installed{}.egraph", eroot);
}

std::filesystem::path store_path(const Invocation& invocation) {
    if (invocation.store) {
        return *invocation.store;
    }
    auto system = system_store_path(invocation);
    if (os::can_create(system)) {
        return system;
    }
    return user_store_path(invocation).value_or(system);
}

std::string builder_program(const Invocation& invocation) {
    if (invocation.builder) {
        return *invocation.builder;
    }
    const auto beside = invocation.program_dir / "egraph-build";
    std::error_code error;
    if (!invocation.program_dir.empty() && std::filesystem::exists(beside, error)) {
        return beside.string();
    }
    return "egraph-build";
}

namespace {

void add_roots(std::vector<std::string>& argv, const Invocation& invocation) {
    argv.insert(argv.end(), {"--root", invocation.root.string()});
    if (invocation.config_root) {
        argv.insert(argv.end(), {"--config-root", invocation.config_root->string()});
    }
    if (invocation.eprefix) {
        argv.insert(argv.end(), {"--eprefix", invocation.eprefix->string()});
    }
}

} // namespace

std::vector<std::string> builder_command(const Invocation& invocation, std::string_view mode,
                                         const std::filesystem::path& path) {
    std::vector<std::string> argv{builder_program(invocation), std::string{mode}, "--store",
                                  path.string()};
    add_roots(argv, invocation);
    return argv;
}

std::vector<std::string> pending_command(const Invocation& invocation,
                                         const std::filesystem::path& output,
                                         const std::vector<std::string>& entries) {
    std::vector<std::string> argv{builder_program(invocation), "--pending", "--output",
                                  output.string()};
    add_roots(argv, invocation);
    argv.insert(argv.end(), entries.begin(), entries.end());
    return argv;
}

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    Session session{invocation, err};
    return std::visit(
        [&](const auto& command) { return execute(command, session, invocation, out, err); },
        invocation.command);
}

} // namespace egraph
