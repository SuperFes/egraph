#include "cli.hpp"

#include "action.hpp"
#include "affected.hpp"

#include "atom.hpp"
#include "blockers.hpp"
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
#include "package_use.hpp"
#include "pressure.hpp"
#include "remove.hpp"
#include "request.hpp"
#include "selection.hpp"
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

// updates' options, for C: Updates or a command extending it.
template <class C> void add_updates_options(CLI::App* sub, Invocation& invocation) {
    const auto updates = [&invocation]() -> Updates& { return std::get<C>(invocation.command); };
    sub->add_flag_callback(
        "--held", [updates] { updates().held = true; },
        "Also the updates installed dependents hold back, which of their atoms do, and the "
        "remedies");
    sub->add_flag_callback(
        "-t,--table", [updates] { updates().table = true; },
        "In merge order, each with the places of the merges it waits for");
    sub->add_flag_callback(
        "--tree", [updates] { updates().tree = true; },
        "Each merge under the root set and the packages it comes from");
    sub->add_flag_callback(
        "--world", [updates] { updates().world = true; },
        "Only the packages the root sets keep, as emerge -u @world");
    sub->add_flag_callback(
        "-D,--deep", [updates] { updates().deep = true; },
        "Every package in scope, not only the arguments and what their merges need, as emerge "
        "--deep");
    sub->add_flag_callback(
        "-N,--newuse", [updates] { updates().rebuilds = UseRebuilds::all; },
        "Also the rebuilds emerge --newuse makes for changed USE or IUSE");
    sub->add_flag_callback(
        "-U,--changed-use",
        [updates] {
            auto& rebuilds = updates().rebuilds;
            // --newuse takes in --changed-use's, as in emerge.
            rebuilds = rebuilds == UseRebuilds::all ? rebuilds : UseRebuilds::changed;
        },
        "Also the rebuilds emerge --changed-use makes for changed USE");
    // An action always verifies.
    if constexpr (std::is_same_v<C, Updates>) {
        sub->add_flag_callback(
            "--verify", [updates] { updates().verify = true; },
            "Also ask emerge --pretend, and show where its merge list differs");
    }
}

// plan's options, for C: PlanCommand or a command extending it.
template <class C> void add_plan_options(CLI::App* sub, Invocation& invocation) {
    const auto plan = [&invocation]() -> PlanCommand& { return std::get<C>(invocation.command); };
    sub->add_option_function<std::vector<std::string>>(
           "targets", [plan](const std::vector<std::string>& targets) { plan().targets = targets; },
           "Atoms and sets (@world, @selected, @system, @profile, @installed), as emerge's")
        ->type_name("PACKAGE")
        ->required();
    sub->add_flag_callback(
        "-u,--update", [plan] { plan().update = true; },
        "Update each installed slot a target matches, as emerge -u");
    sub->add_flag_callback(
        "-D,--deep", [plan] { plan().deep = true; },
        "Update every package in scope, not only the targets and what their merges need, as "
        "emerge --deep (with -u)");
    sub->add_flag_callback(
        "-n,--noreplace", [plan] { plan().noreplace = true; },
        "Skip a target something installed matches, as emerge --noreplace");
    sub->add_flag_callback(
        "-N,--newuse", [plan] { plan().rebuilds = UseRebuilds::all; },
        "Also the rebuilds emerge --newuse makes for changed USE or IUSE (with -u)");
    sub->add_flag_callback(
        "-U,--changed-use",
        [plan] {
            auto& rebuilds = plan().rebuilds;
            rebuilds = rebuilds == UseRebuilds::all ? rebuilds : UseRebuilds::changed;
        },
        "Also the rebuilds emerge --changed-use makes for changed USE (with -u)");
    sub->add_flag_callback(
        "-t,--table", [plan] { plan().table = true; },
        "In merge order, each with the places of the merges it waits for");
    if constexpr (std::is_same_v<C, PlanCommand>) {
        sub->add_flag_callback(
            "--verify", [plan] { plan().verify = true; },
            "Also ask emerge --pretend, and show where its merge list differs");
    }
}

// An action's --yes.
template <class C> void add_yes(CLI::App* sub, Invocation& invocation) {
    sub->add_flag_callback(
        "-y,--yes", [&invocation] { std::get<C>(invocation.command).yes = true; },
        "Run emerge without asking, as scripts must where there is no terminal to ask on");
}

// Where a command line was typed: the shell, or the interface's prompt.
enum class Context : std::uint8_t { shell, interface };

// What a command line did: ran a command, with its status, or asked to quit.
struct LineResult {
    bool quit = false;
    Exit exit = Exit::ok;
};

Exit dispatch(Session& session, const Invocation& invocation, std::ostream& out, std::ostream& err);
LineResult run_line(Session& session, const Invocation& invocation, std::string_view line,
                    std::ostream& out, std::ostream& err, Context context);
Exit run_shell(Session& session, const Invocation& invocation, std::istream& in, std::ostream& out,
               std::ostream& err, bool prompt);
Exit execute(const Tui& command, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err);

// No command: interactive, in the interface when both ends are a terminal, else the shell.
Exit execute(const std::monostate&, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    if (tui::available() && invocation.terminal && invocation.input_terminal) {
        return execute(Tui{}, session, invocation, out, err);
    }
    return run_shell(session, invocation, std::cin, out, err, invocation.input_terminal);
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

// A file removed with this.
class ScratchFile {
  public:
    explicit ScratchFile(std::filesystem::path path) : path_{std::move(path)} {}
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&& other) noexcept : path_{std::exchange(other.path_, {})} {}
    ScratchFile& operator=(ScratchFile&&) = delete;
    ~ScratchFile() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

// A full build into path in the background, its output kept for errors rather than sent to the
// terminal: the job ends with the error, which ends with the output's last lines, or with
// nothing once the build succeeded.
Job<std::optional<std::string>> background_build(const Invocation& invocation,
                                                 std::string_view mode,
                                                 const std::filesystem::path& path) {
    ScratchFile log{std::filesystem::path{scratch_store()}.replace_extension(".log")};
    auto child = os::start(builder_command(invocation, mode, path), log.path());
    if (!child) {
        return ready(builder_error(invocation, std::unexpected(child.error())));
    }
    return [&invocation, child = std::move(*child),
            log = std::move(log)]() mutable -> std::optional<std::optional<std::string>> {
        const auto ended = child.poll();
        if (!ended) {
            return std::nullopt;
        }
        auto error = builder_error(invocation, *ended);
        if (const auto output = last_lines(log.path(), 8); error && !output.empty()) {
            *error += ":\n" + output;
        }
        return std::optional<std::optional<std::string>>{std::in_place, std::move(error)};
    };
}

// A full build into scratch files, loaded by load_path (Loaded is a Store or Stores), the files
// removed once loaded.
template <class Loaded, class Load>
std::expected<Loaded, std::string> fresh_build(const Invocation& invocation,
                                               const Load& load_path) {
    const ScratchStores files{scratch_store()};
    if (auto error = run_builder(invocation, "--full", files.path())) {
        return std::unexpected(std::move(*error));
    }
    return load_path(files.path()).transform_error([](const StoreError& e) { return e.message; });
}

// The check's fresh build in the background, and how stored differs from it; with keep, its
// files are left in checked for a rebuild to save.
Job<tui::CheckResult> background_check(const Invocation& invocation, const Store& stored,
                                       std::optional<ScratchStores>& checked, bool keep) {
    checked.reset();
    return [files = ScratchStores{scratch_store()}, build = Job<std::optional<std::string>>{},
            &invocation, &stored, &checked, keep]() mutable -> std::optional<tui::CheckResult> {
        if (!build) {
            build = background_build(invocation, "--full", files.path());
        }
        auto ended = build();
        if (!ended) {
            return std::nullopt;
        }
        if (*ended) {
            return tui::CheckResult{std::unexpected(std::move(**ended))};
        }
        auto loaded = load_stores(files.path());
        if (!loaded) {
            return tui::CheckResult{std::unexpected(std::move(loaded.error().message))};
        }
        auto lines = drift(stored, loaded->installed);
        if (keep) {
            checked = std::move(files);
        }
        return tui::Fresh{.store = std::move(loaded->installed),
                          .evaluated = std::move(loaded->evaluated),
                          .drift = std::move(lines)};
    };
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

Job<std::expected<Stores, std::string>> save_stores(const Invocation& invocation,
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
                    return ready(std::expected<Stores, std::string>{std::unexpected(
                        std::format("{}: {}", to.string(), copied.error().message()))});
                }
            }
            return ready(std::expected<Stores, std::string>{std::move(*stores)});
        }
    }
    return [build = background_build(invocation, "--full", path),
            path]() mutable -> std::optional<std::expected<Stores, std::string>> {
        auto ended = build();
        if (!ended) {
            return std::nullopt;
        }
        if (*ended) {
            return std::expected<Stores, std::string>{std::unexpected(std::move(**ended))};
        }
        return load_stores(path).transform_error([](const StoreError& e) { return e.message; });
    };
}

namespace {

// Stores loaded from used, which the session answers from as well from now on.
std::shared_ptr<const Stores> share(Session& session, Stores stores, std::filesystem::path used) {
    auto shared = std::make_shared<const Stores>(std::move(stores));
    session.adopt(shared, std::move(used));
    return shared;
}

// Brings the stores up to date in the background, as a session opens them: a current system
// store, or the user's after an incremental build. The session answers from them too.
Job<tui::RefreshResult> background_refresh(const Invocation& invocation, Session& session) {
    std::filesystem::path used;
    if (auto current = current_stores(invocation, used)) {
        return ready(tui::RefreshResult{share(session, std::move(*current), used)});
    }
    return [build = background_build(invocation, "--incremental", used), used,
            &session]() mutable -> std::optional<tui::RefreshResult> {
        auto ended = build();
        if (!ended) {
            return std::nullopt;
        }
        if (*ended) {
            return tui::RefreshResult{std::unexpected(std::move(**ended))};
        }
        auto loaded = load_stores(used);
        if (!loaded) {
            return tui::RefreshResult{std::unexpected(std::move(loaded.error().message))};
        }
        return share(session, std::move(*loaded), used);
    };
}

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
    const auto built = fresh_build<Store>(invocation, [](const auto& path) { return load(path); });
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

// Holds plan to what emerge --pretend merges for request, and shows where they differ.
// Whether the plan is for the running system: emerge treats strong blockers differently there.
bool running_root(const Invocation& invocation) {
    return invocation.root.lexically_normal() == "/";
}

// A plan's exit status once shown: refused when emerge would refuse it and nothing else went
// wrong.
Exit finish(Exit status, const Plan& plan) {
    return status == Exit::ok && plan.refused() ? Exit::refused : status;
}

Exit verify(const Invocation& invocation, std::string_view command, const EmergeRequest& request,
            const Store& store, const Evaluated& evaluated, const Plan& plan, std::ostream& out,
            std::ostream& err) {
    const auto style = output(invocation);
    out << std::flush;
    const auto printed = output_of(emerge_command(invocation, request));
    // emerge prints the list before refusing it for blockers it cannot resolve, and names what
    // it cannot satisfy.
    const auto listed = parse_pretend(printed ? *printed : printed.error(), !printed);
    if (!printed && listed.blocks.empty() && listed.unsatisfied.empty() && listed.unmet.empty() &&
        listed.use_changes.empty()) {
        // emerge explains itself at length; its last lines say why.
        constexpr std::size_t shown = 20;
        std::string_view text = printed.error();
        auto start = text.size();
        for (std::size_t lines = 0; lines < shown && start > 0; ++lines) {
            start = text.rfind('\n', start - 1);
            if (start == std::string_view::npos) {
                start = 0;
                break;
            }
        }
        err << "egraph: " << command << ": emerge --pretend failed:\n"
            << text.substr(start == 0 ? 0 : start + 1) << '\n';
        return Exit::failure;
    }
    const auto differences = merge_differences(planned_merges(store, evaluated, plan), listed);
    if (style.human) {
        human_verification(out, differences, style.theme);
    } else if (!differences.empty()) {
        err << "egraph: " << command << ": emerge --pretend merges otherwise:\n";
        write_lines(err, differences);
    }
    return differences.empty() ? Exit::ok : Exit::differs;
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

Exit execute(const Blockers& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto loaded = session.dependencies(invocation.dynamic_deps);
    if (!loaded) {
        return fail(err, loaded.error());
    }
    const Store& store = *loaded;
    const auto ids = resolve_all(store, command.packages, err);
    if (!ids) {
        return Exit::failure;
    }
    const auto lines = blocker_lines(store, *ids);
    if (const auto style = output(invocation); style.human) {
        human_blockers(out, lines, !ids->empty(), style.theme);
    } else {
        write_lines(out, lines);
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

// When the invocation may ask, offers to write the plan's USE changes to package.use; once they
// are, replan runs the command again, offering no more, on stores refreshed for them.
template <typename Replan>
Exit offer_use_changes(Exit status, const Plan& plan, const Store& store,
                       const Evaluated& evaluated, Session& session, const Invocation& invocation,
                       std::ostream& out, std::ostream& err, const Replan& replan) {
    if (!invocation.ask || invocation.use_offered || plan.use_changes.empty() ||
        !output(invocation).human) {
        return status;
    }
    const auto path = package_use_path(
        invocation.config_root.value_or(invocation.eprefix.value_or(std::filesystem::path{"/"})));
    if (!answered_yes(std::cin, out, std::format("Write the USE changes to {}?", path.string()))) {
        return status;
    }
    if (const auto error = append_to_file(path, package_use_text(store, evaluated, plan))) {
        err << "egraph: " << *error << '\n';
        return Exit::failure;
    }
    out << "Wrote " << path.string() << "; planning again.\n";
    session.reload();
    auto again = invocation;
    again.use_offered = true;
    return replan(again);
}

// A plan as shown, and the request that has emerge plan it too.
struct Shown {
    Plan plan;
    EmergeRequest request;
    // The session's, until it reloads.
    std::reference_wrapper<const Store> store;
    std::reference_wrapper<const Evaluated> evaluated;
};

// The updates, shown; the exit status instead when there are none to show.
std::expected<Shown, Exit> show_updates(const Updates& command, Session& session,
                                        const Invocation& invocation, std::ostream& out,
                                        std::ostream& err) {
    // Both stores first, so that the dependencies are read from the same build.
    const auto stores = session.stores();
    if (!stores) {
        return std::unexpected(fail(err, stores.error()));
    }
    const auto store = session.dependencies(invocation.dynamic_deps);
    if (!store) {
        return std::unexpected(fail(err, store.error()));
    }
    const auto& evaluated = stores->get().evaluated;
    const auto depclean = command.tree || command.world
                              ? std::optional{session.depclean(true, invocation.dynamic_deps)}
                              : std::nullopt;
    if (depclean && !*depclean) {
        return std::unexpected(fail(err, depclean->error()));
    }
    auto targets = command.world ? Targets{.scope = (*depclean)->get().kept.packages,
                                           .roots = true,
                                           .deep = command.deep}
                                 : Targets{.scope = {}, .roots = false, .deep = command.deep};
    targets.running_root = running_root(invocation);
    Shown shown{.plan = plan_updates(*store, evaluated, command.rebuilds, targets),
                .request = {.targets = {command.world ? "@world" : "@installed"},
                            .update = true,
                            .deep = command.deep,
                            .rebuilds = command.rebuilds,
                            .dynamic_deps = invocation.dynamic_deps},
                .store = *store,
                .evaluated = evaluated};
    const auto& plan = shown.plan;
    if (command.tree) {
        const auto tree = update_tree_lines(*store, evaluated, (*depclean)->get().kept, plan);
        if (const auto style = output(invocation); style.human) {
            human_update_tree(
                out, update_lines(*store, evaluated, plan, command.rebuilds, false, true, targets),
                tree, style.theme);
        } else {
            write_lines(out, tree);
        }
        return shown;
    }
    std::optional<RemedyInputs> remedies;
    if (command.held) {
        const auto graph = session.graph(invocation.dynamic_deps);
        if (!graph) {
            return std::unexpected(fail(err, graph.error()));
        }
        remedies = RemedyInputs{.graph = *graph, .rescope = {}};
        if (command.world) {
            const auto& found = (*depclean)->get();
            remedies->rescope = [&found](const std::vector<bool>& removed) {
                auto options = found.options;
                options.removed = removed;
                return keep(found.store, options).packages;
            };
        }
    }
    const auto lines = update_lines(*store, evaluated, plan, command.rebuilds, command.held,
                                    command.table, targets, remedies);
    if (const auto style = output(invocation); style.human) {
        human_updates(out, lines, style.theme, command.table);
    } else {
        write_lines(out, lines);
    }
    return shown;
}

Exit execute(const Updates& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto shown = show_updates(command, session, invocation, out, err);
    if (!shown) {
        return shown.error();
    }
    const auto& [plan, request, store, evaluated] = *shown;
    const auto status = finish(command.verify ? verify(invocation, Updates::name, request, store,
                                                       evaluated, plan, out, err)
                                              : Exit::ok,
                               plan);
    return offer_use_changes(
        status, plan, store, evaluated, session, invocation, out, err,
        [&](const Invocation& again) { return execute(command, session, again, out, err); });
}

// The request the targets name, the cps only the repositories know evaluated first.
std::expected<Request, std::string> resolve_request(const PlanCommand& command, Session& session,
                                                    const Invocation& invocation) {
    for (bool evaluated = false;; evaluated = true) {
        const auto stores = session.stores();
        if (!stores) {
            return std::unexpected(stores.error());
        }
        const auto store = session.dependencies(invocation.dynamic_deps);
        if (!store) {
            return std::unexpected(store.error());
        }
        auto request = parse_request(*store, stores->get().evaluated, command.targets);
        if (!request || request->unevaluated.empty()) {
            return request;
        }
        std::string cps;
        for (const auto& cp : request->unevaluated) {
            cps += std::format("{}{}", cps.empty() ? "" : ", ", cp);
        }
        if (evaluated) {
            return std::unexpected(std::format("{}: egraph-build did not evaluate it", cps));
        }
        if (invocation.no_refresh) {
            return std::unexpected(std::format(
                "{}: not evaluated yet, and --no-refresh keeps egraph-build from evaluating it",
                cps));
        }
        if (auto error = session.evaluate(request->unevaluated)) {
            return std::unexpected(std::move(*error));
        }
    }
}

// The plan for a request, shown; the exit status instead when there is none to show.
std::expected<Shown, Exit> show_plan(const PlanCommand& command, std::string_view name,
                                     Session& session, const Invocation& invocation,
                                     std::ostream& out, std::ostream& err) {
    if (!command.update && (command.deep || command.rebuilds != UseRebuilds::none)) {
        err << "egraph: " << name << ": -D, -N and -U are only planned with -u\n";
        return std::unexpected(Exit::usage);
    }
    const auto request = resolve_request(command, session, invocation);
    if (!request) {
        err << "egraph: " << name << ": " << request.error() << '\n';
        return std::unexpected(Exit::failure);
    }
    // Loaded again, as evaluating replaced them.
    const auto stores = session.stores();
    if (!stores) {
        return std::unexpected(fail(err, stores.error()));
    }
    const auto store = session.dependencies(invocation.dynamic_deps);
    if (!store) {
        return std::unexpected(fail(err, store.error()));
    }
    const auto& evaluated = stores->get().evaluated;
    // An empty set asks for nothing.
    if (!request->installed && request->arguments.empty()) {
        return std::unexpected(Exit::ok);
    }
    const auto selection = command.update      ? Selection::update
                           : command.noreplace ? Selection::noreplace
                                               : Selection::reinstall;
    Targets targets{.scope = {}, .roots = false, .deep = command.deep, .selection = selection};
    targets.running_root = running_root(invocation);
    if (request->installed) {
        if (!request->arguments.empty() || selection != Selection::update) {
            err << "egraph: " << name << ": @installed is only planned alone and with -u\n";
            return std::unexpected(Exit::usage);
        }
    } else {
        const auto depclean = session.depclean(true, invocation.dynamic_deps);
        if (!depclean) {
            return std::unexpected(fail(err, depclean.error()));
        }
        // emerge completes its graph with @world, so the arguments' reach weighs beside it.
        targets.reach = request_reach(*store, evaluated, *request);
        targets.scope = depclean->get().kept.packages;
        for (std::size_t id = 0; id < targets.scope.size(); ++id) {
            targets.scope.at(id) = targets.scope.at(id) || targets.reach.at(id);
        }
        targets.roots = true;
        targets.request = request->arguments;
    }
    Shown shown{.plan = plan_updates(*store, evaluated, command.rebuilds, targets),
                .request = {.targets = command.targets,
                            .update = command.update,
                            .deep = command.deep,
                            .noreplace = command.noreplace,
                            .rebuilds = command.rebuilds,
                            .dynamic_deps = invocation.dynamic_deps},
                .store = *store,
                .evaluated = evaluated};
    const auto lines = update_lines(*store, evaluated, shown.plan, command.rebuilds, false,
                                    command.table, targets);
    if (const auto style = output(invocation); style.human) {
        human_updates(out, lines, style.theme, command.table);
    } else {
        write_lines(out, lines);
    }
    return shown;
}

Exit execute(const PlanCommand& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto shown = show_plan(command, PlanCommand::name, session, invocation, out, err);
    if (!shown) {
        return shown.error();
    }
    const auto& [plan, request, store, evaluated] = *shown;
    const auto status = finish(command.verify ? verify(invocation, PlanCommand::name, request,
                                                       store, evaluated, plan, out, err)
                                              : Exit::ok,
                               plan);
    return offer_use_changes(
        status, plan, store, evaluated, session, invocation, out, err,
        [&](const Invocation& again) { return execute(command, session, again, out, err); });
}

// EMERGE_DEFAULT_OPTS's options that change only how emerge runs, through egraph-build.
std::expected<std::vector<std::string>, std::string> passed_options(const Invocation& invocation) {
    const auto output = std::filesystem::path{scratch_store()}.replace_extension(".options");
    const auto ran = output_of(emerge_options_command(invocation, output));
    std::ifstream in{output};
    std::vector<std::string> words;
    for (std::string word; std::getline(in, word);) {
        words.push_back(std::move(word));
    }
    in.close();
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    if (!ran) {
        return std::unexpected(ran.error());
    }
    return execution_options(words);
}

// An action: the plan show() shows, verified against emerge --pretend, confirmed, and emerge run
// on it, the stores refreshed after; act runs the action again once USE changes are written.
// How an action speaks of what it does: "merge", "merging", "merged", or the same of removing.
struct ActionWords {
    std::string_view verb;
    std::string_view gerund;
    std::string_view past;
};
constexpr ActionWords merge_words{.verb = "merge", .gerund = "merging", .past = "merged"};
constexpr ActionWords remove_words{.verb = "remove", .gerund = "removing", .past = "removed"};

// The exit status when an action stops before asking emerge, saying why.
std::optional<Exit> stopped(std::optional<Stop> stop, std::string_view name,
                            const ActionWords& words, const std::filesystem::path& vdb, bool human,
                            std::ostream& out, std::ostream& err) {
    if (!stop) {
        return std::nullopt;
    }
    switch (*stop) {
    case Stop::refused:
        err << "egraph: " << name << ": emerge would refuse the plan; nothing was " << words.past
            << '\n';
        return Exit::refused;
    case Stop::nothing:
        // In the lines layout, no lines say it.
        if (human) {
            out << "Nothing to " << words.verb << ".\n";
        }
        return Exit::ok;
    case Stop::unprivileged:
        err << "egraph: " << name << ": " << words.gerund << " needs write access to "
            << vdb.string() << "; run egraph as root\n";
        return Exit::failure;
    case Stop::unconfirmed:
        err << "egraph: " << name << ": no terminal to ask on; give --yes to have emerge "
            << words.verb << " without asking\n";
        return Exit::usage;
    }
    return Exit::failure;
}

// The installed packages' database under the store's EROOT.
std::filesystem::path vdb_of(const Store& store) {
    return std::filesystem::path{store.meta.eroot} / "var/db/pkg";
}

// An action once verified: asks unless yes, runs emerge with arguments(passed), passed being
// EMERGE_DEFAULT_OPTS' execution options, and refreshes the stores after.
template <class Arguments>
Exit confirm_and_run(std::string_view name, bool yes, std::string_view question,
                     const Arguments& arguments, const Store& store, Session& session,
                     const Invocation& invocation, std::ostream& out, std::ostream& err) {
    // Copied before the session reloads, which takes store with it.
    const auto selected = world_atoms(store);
    const auto passed = passed_options(invocation);
    if (!passed) {
        err << "egraph: " << name << ": EMERGE_DEFAULT_OPTS could not be read: " << passed.error()
            << '\n';
        return Exit::failure;
    }
    if (!yes && !answered_yes(std::cin, out, question)) {
        return Exit::failure;
    }
    out << std::flush;
    const auto ran = os::run(emerge_command(invocation, arguments(*passed)));
    // What emerge did before it failed counts too.
    session.reload();
    if (const auto stores = session.stores(); !stores) {
        err << "egraph: " << name << ": the stores could not be refreshed: " << stores.error()
            << '\n';
    } else {
        const auto changes = selection_changes(selected, world_atoms(stores->get().installed));
        if (const auto style = output(invocation); style.human) {
            human_selection(out, changes, style.theme);
        } else {
            write_lines(out, changes);
        }
    }
    if (!ran) {
        err << "egraph: " << name << ": " << ran.error().message << '\n';
        return Exit::failure;
    }
    if (*ran != 0) {
        err << "egraph: " << name << ": emerge exited with status " << *ran << '\n';
        return Exit::failure;
    }
    return Exit::ok;
}

// An action merging a plan: the plan show() shows, verified against emerge --pretend, confirmed,
// and emerge run on it; act runs the action again once USE changes are written.
template <class Show, class Act>
Exit run_action(std::string_view name, bool oneshot, bool yes, Session& session,
                const Invocation& invocation, std::ostream& out, std::ostream& err,
                const Show& show, const Act& act,
                std::string_view question = "Have emerge merge this plan?",
                bool selecting = false) {
    const auto shown = show();
    if (!shown) {
        return shown.error();
    }
    const auto& [plan, request, store, evaluated] = *shown;
    if (!yes && !plan.use_changes.empty()) {
        const auto status = offer_use_changes(Exit::refused, plan, store, evaluated, session,
                                              invocation, out, err, act);
        if (status != Exit::refused) {
            return status;
        }
    }
    const auto vdb = vdb_of(store);
    if (const auto status =
            stopped(stop_before_verifying(
                        {.refused = plan.refused(),
                         // Selecting needs no merge.
                         .empty = !selecting && plan.merges.empty() && plan.uninstalls.empty(),
                         .writable = os::can_create(vdb / "egraph"),
                         .yes = yes,
                         .can_ask = invocation.ask}),
                    name, merge_words, vdb, output(invocation).human, out, err)) {
        return *status;
    }
    if (const auto status = verify(invocation, name, request, store, evaluated, plan, out, err);
        status != Exit::ok) {
        if (status == Exit::differs) {
            err << "egraph: " << name << ": emerge would merge otherwise; nothing was merged\n";
        }
        return status;
    }
    return confirm_and_run(
        name, yes, question,
        [&](const std::vector<std::string>& passed) {
            return run_arguments(request, oneshot, passed);
        },
        store, session, invocation, out, err);
}

Exit execute(const Update& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    return run_action(
        Update::name, true, command.yes, session, invocation, out, err,
        [&] { return show_updates(command, session, invocation, out, err); },
        [&](const Invocation& again) { return execute(command, session, again, out, err); });
}

Exit execute(const Install& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    return run_action(
        Install::name, command.oneshot, command.yes, session, invocation, out, err,
        [&] { return show_plan(command, Install::name, session, invocation, out, err); },
        [&](const Invocation& again) { return execute(command, session, again, out, err); });
}

// A remove's targets as emerge takes them: an exact cpv as =cpv.
std::vector<std::string> depclean_targets(const Store& store,
                                          const std::vector<std::string>& packages) {
    std::vector<std::string> targets;
    targets.reserve(packages.size());
    for (const auto& package : packages) {
        const auto exact = std::ranges::any_of(
            store.packages, [&](const Package& pkg) { return store.string(pkg.cpv) == package; });
        targets.push_back(exact ? "=" + package : package);
    }
    return targets;
}

Exit execute(const Remove& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto depclean = session.depclean(command.build_deps, invocation.dynamic_deps);
    if (!depclean) {
        return fail(err, depclean.error());
    }
    const Store& store = depclean->get().store;
    // depclean refuses to run without one.
    if (store.roots.empty()) {
        err << "egraph: remove: the @world set is empty\n";
        return Exit::failure;
    }
    const auto matched = resolve_all(store, command.packages, err);
    if (!matched) {
        return Exit::failure;
    }
    const auto removal = plan_removal(store, depclean->get().options, *matched);
    const auto lines = removal_lines(store, removal);
    const auto style = output(invocation);
    if (style.human) {
        human_removal(out, lines, style.theme);
    } else {
        write_lines(out, lines);
    }
    const auto vdb = vdb_of(store);
    if (const auto status =
            stopped(stop_before_verifying({.refused = false,
                                           .empty = removal.removed.empty(),
                                           .writable = os::can_create(vdb / "egraph"),
                                           .yes = command.yes,
                                           .can_ask = invocation.ask}),
                    Remove::name, remove_words, vdb, style.human, out, err)) {
        return *status;
    }
    auto options = depclean_options(command.build_deps, invocation.dynamic_deps);
    const auto targets = depclean_targets(store, command.packages);
    std::vector<std::string> pretend{"--pretend", "--depclean", "--color=n", "--nospinner",
                                     "--ignore-default-opts"};
    pretend.insert(pretend.end(), options.begin(), options.end());
    pretend.insert(pretend.end(), targets.begin(), targets.end());
    out << std::flush;
    const auto printed = output_of(emerge_command(invocation, pretend));
    if (!printed) {
        err << "egraph: remove: emerge --pretend --depclean failed:\n" << printed.error() << '\n';
        return Exit::failure;
    }
    const auto differences = removal_differences(store, removal, parse_depclean(*printed));
    if (style.human) {
        human_verification(out, differences, style.theme, "removes");
    }
    if (!differences.empty()) {
        if (!style.human) {
            err << "egraph: remove: emerge --pretend --depclean removes otherwise:\n";
            write_lines(err, differences);
        }
        err << "egraph: remove: emerge would remove otherwise; nothing was removed\n";
        return Exit::differs;
    }
    return confirm_and_run(
        Remove::name, command.yes, "Have emerge remove these packages?",
        [&](const std::vector<std::string>& passed) {
            std::vector<std::string> arguments{"--depclean", "--ignore-default-opts", "--ask=n"};
            arguments.insert(arguments.end(), passed.begin(), passed.end());
            arguments.insert(arguments.end(), options.begin(), options.end());
            arguments.insert(arguments.end(), targets.begin(), targets.end());
            return arguments;
        },
        store, session, invocation, out, err);
}

Exit execute(const Select& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    PlanCommand plan;
    plan.targets = command.packages;
    plan.noreplace = true;
    return run_action(
        Select::name, false, command.yes, session, invocation, out, err,
        [&] { return show_plan(plan, Select::name, session, invocation, out, err); },
        [&](const Invocation& again) { return execute(command, session, again, out, err); },
        "Have emerge add these to @selected, merging what is not installed?", true);
}

Exit execute(const Deselect& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto depclean = session.depclean(true, invocation.dynamic_deps);
    if (!depclean) {
        return fail(err, depclean.error());
    }
    const Store& store = depclean->get().store;
    // What emerge would remove is its own to say: it matches world atoms its own way.
    std::vector<std::string> pretend{"--pretend", "--deselect", "--color=n", "--nospinner",
                                     "--ignore-default-opts"};
    pretend.insert(pretend.end(), command.packages.begin(), command.packages.end());
    out << std::flush;
    const auto printed = output_of(emerge_command(invocation, pretend));
    if (!printed) {
        err << "egraph: deselect: emerge --pretend --deselect failed:\n" << printed.error() << '\n';
        return Exit::failure;
    }
    const auto atoms = parse_deselect(*printed);
    const auto lines = deselect_lines(store, depclean->get().options, atoms);
    const auto style = output(invocation);
    if (!atoms.empty()) {
        if (style.human) {
            human_deselect(out, lines, style.theme);
        } else {
            write_lines(out, lines);
        }
    }
    const auto world = std::filesystem::path{store.meta.eroot} / "var/lib/portage/world";
    constexpr ActionWords deselect_words{
        .verb = "deselect", .gerund = "deselecting", .past = "deselected"};
    if (const auto status = stopped(stop_before_verifying({.refused = false,
                                                           .empty = atoms.empty(),
                                                           .writable = os::can_create(world),
                                                           .yes = command.yes,
                                                           .can_ask = invocation.ask}),
                                    Deselect::name, deselect_words, world, style.human, out, err)) {
        return *status;
    }
    return confirm_and_run(
        Deselect::name, command.yes, "Have emerge remove these from @selected?",
        [&](const std::vector<std::string>& passed) {
            std::vector<std::string> arguments{"--deselect", "--ignore-default-opts", "--ask=n"};
            arguments.insert(arguments.end(), passed.begin(), passed.end());
            arguments.insert(arguments.end(), command.packages.begin(), command.packages.end());
            return arguments;
        },
        store, session, invocation, out, err);
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

Exit execute(const Tui&, Session& session, const Invocation& invocation, std::ostream&,
             std::ostream& err) {
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
    session.warn_to(warned);
    const auto stores = session.shared_stores();
    session.warn_to(err);
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
    const auto check = [&invocation, &checked, saves](const Store& stored) {
        return background_check(invocation, stored, checked, saves);
    };
    tui::Rebuilder rebuild;
    if (saves) {
        rebuild = [&invocation, &checked, &session]() -> Job<tui::RebuildResult> {
            auto saved = save_stores(invocation, checked);
            checked.reset();
            return [saved = std::move(saved), &invocation,
                    &session]() mutable -> std::optional<tui::RebuildResult> {
                auto result = saved();
                if (!result) {
                    return std::nullopt;
                }
                if (!*result) {
                    return tui::RebuildResult{std::unexpected(std::move(result->error()))};
                }
                return share(session, std::move(**result), store_path(invocation));
            };
        };
    }
    const auto run_dir = emerge::status_dir(invocation.eprefix.value_or(""));
    const auto watch = [run_dir] { return emerge::read_snapshots(run_dir); };
    const auto sample = [] { return pressure::read_sample(); };
    const auto set_steve = [](steve::Setting setting,
                              double value) -> std::expected<void, std::string> {
        return output_of(steve::set_arguments(setting, value)).transform([](const auto&) {});
    };
    // What a command at the prompt prints, and its warnings, go to its answer.
    const auto command = [&session, &invocation](const std::string& line) {
        std::ostringstream out;
        std::ostringstream problems;
        session.warn_to(problems);
        const auto result = run_line(session, invocation, line, out, problems, Context::interface);
        return tui::Answer{
            .exit = result.exit, .out = out.str(), .err = problems.str(), .quit = result.quit};
    };
    // --no-refresh keeps the stores as opened.
    tui::Staleness stale;
    tui::Refresher refresh;
    if (!invocation.no_refresh) {
        stale = [](const Stores& shown) { return staleness(shown); };
        refresh = [&invocation, &session] { return background_refresh(invocation, session); };
    }
    return tui::open_and_run(
        *stores, invocation.dynamic_deps, style(invocation).glyphs,
        {.check = check,
         .rebuild = rebuild,
         .watch = watch,
         .sample = sample,
         .steve = read_steve,
         .set_steve = set_steve,
         .merge_list = [&invocation] { return read_merge_list(invocation); },
         .plan = [&invocation](
                     const std::vector<emerge::Pending>& list) { return plan(invocation, list); },
         .command = command,
         .stale = stale,
         .refresh = refresh,
         .now = {}},
        warnings, err);
}

Exit execute(const Shell&, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    return run_shell(session, invocation, std::cin, out, err, invocation.input_terminal);
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
    // Without one, egraph is interactive.
    app.require_subcommand(0, 1);

    app.add_option("--root", invocation.root, "Root whose installed packages to query")
        ->type_name("DIR")
        ->envname("ROOT")
        ->capture_default_str();
    app.add_option("--config-root", invocation.config_root,
                   "Root of the portage configuration to evaluate them with")
        ->type_name("DIR")
        ->envname("PORTAGE_CONFIGROOT");
    app.add_option("--eprefix", invocation.eprefix, "Offset prefix of a prefix installation")
        ->type_name("DIR")
        ->envname("PORTAGE_OVERRIDE_EPREFIX");
    app.add_option("--store", invocation.store,
                   "Store file to read (default: ${ROOT}${EPREFIX}/var/cache/egraph/"
                   "installed.egraph)")
        ->type_name("FILE")
        ->envname("EGRAPH_STORE");
    app.add_option("--builder", invocation.builder,
                   "egraph-build command that refreshes the store (default: the one next to "
                   "egraph, else egraph-build in PATH)")
        ->type_name("COMMAND")
        ->envname("EGRAPH_BUILD");
    app.add_option("--emerge", invocation.emerge,
                   "emerge command that --verify runs (default: emerge in PATH)")
        ->type_name("COMMAND")
        ->envname("EGRAPH_EMERGE");
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
        ->type_name("PACKAGE")
        ->required();
    deps_cmd->add_flag_callback(
        "--possible", [&invocation] { std::get<Deps>(invocation.command).possible = true; },
        possible_help);
    CLI::App* rdeps_cmd =
        add_dynamic_deps(add_command<Rdeps>(app, invocation, "What depends on installed packages"));
    add_field(rdeps_cmd, invocation, "packages", &Rdeps::packages,
              "Installed cpvs, or cps for every installed version")
        ->type_name("PACKAGE")
        ->required();
    rdeps_cmd->add_flag_callback(
        "--possible", [&invocation] { std::get<Rdeps>(invocation.command).possible = true; },
        possible_help);
    CLI::App* why_cmd = add_dynamic_deps(add_command<Why>(
        app, invocation, "Shortest chain of dependencies from a root set that keeps a package"));
    add_field(why_cmd, invocation, "package", &Why::package,
              "Portage atom; every installed package it matches is explained")
        ->type_name("PACKAGE")
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
    CLI::App* blockers_cmd = add_dynamic_deps(
        add_command<Blockers>(app, invocation, "Blockers between installed packages"));
    add_field(blockers_cmd, invocation, "packages", &Blockers::packages,
              "Installed cpvs, or cps for every installed version; without them, the blockers "
              "that match something installed")
        ->type_name("PACKAGE");
    CLI::App* orphans_cmd = add_dynamic_deps(
        add_command<Orphans>(app, invocation, "Installed packages emerge --depclean would remove"));
    add_field(orphans_cmd, invocation, "--with-bdeps", &Orphans::build_deps,
              "Whether build-time dependencies keep packages, as emerge's option (default y)")
        ->transform(yes_no);

    add_updates_options<Updates>(
        add_dynamic_deps(add_command<Updates>(
            app, invocation, "Installed packages emerge -u would replace or rebuild")),
        invocation);
    add_plan_options<PlanCommand>(
        add_dynamic_deps(add_command<PlanCommand>(
            app, invocation, "What emerge --pretend would merge for a request")),
        invocation);
    CLI::App* update_cmd = add_dynamic_deps(add_command<Update>(
        app, invocation,
        "Show the updates, then have emerge -u --oneshot merge them once confirmed"));
    add_updates_options<Update>(update_cmd, invocation);
    add_yes<Update>(update_cmd, invocation);
    CLI::App* install_cmd = add_dynamic_deps(add_command<Install>(
        app, invocation, "Show the plan for a request, then have emerge merge it once confirmed"));
    add_plan_options<Install>(install_cmd, invocation);
    install_cmd->add_flag_callback(
        "-1,--oneshot", [&invocation] { std::get<Install>(invocation.command).oneshot = true; },
        "Add the targets to no set, as emerge --oneshot");
    add_yes<Install>(install_cmd, invocation);
    CLI::App* remove_cmd = add_dynamic_deps(add_command<Remove>(
        app, invocation,
        "Show what emerge --depclean would remove of the packages, then have it remove them once "
        "confirmed"));
    add_field(remove_cmd, invocation, "packages", &Remove::packages,
              "Portage atoms or installed cpvs; what they match goes unless something keeps it")
        ->type_name("PACKAGE")
        ->required();
    add_field(remove_cmd, invocation, "--with-bdeps", &Remove::build_deps,
              "Whether build-time dependencies keep packages, as emerge's option (default y)")
        ->transform(yes_no);
    add_yes<Remove>(remove_cmd, invocation);
    CLI::App* select_cmd = add_dynamic_deps(add_command<Select>(
        app, invocation,
        "Have emerge add packages to @selected once confirmed, merging those not installed"));
    add_field(select_cmd, invocation, "packages", &Select::packages, "Atoms and sets, as emerge's")
        ->type_name("PACKAGE")
        ->required();
    add_yes<Select>(select_cmd, invocation);
    CLI::App* deselect_cmd = add_command<Deselect>(
        app, invocation,
        "Show what emerge --deselect would remove from @selected and what depclean would then "
        "remove, then have it deselect them once confirmed");
    add_field(deselect_cmd, invocation, "packages", &Deselect::packages,
              "Atoms and sets, as emerge's")
        ->type_name("PACKAGE")
        ->required();
    add_yes<Deselect>(deselect_cmd, invocation);

    CLI::App* export_cmd = add_command<Export>(app, invocation, "Export part of the graph");
    add_field(export_cmd, invocation, "--format", &Export::format, "Output format")
        ->transform(
            one_of<ExportFormat>({{"dot", ExportFormat::dot}, {"json", ExportFormat::json}}, true));
    add_field(export_cmd, invocation, "packages", &Export::packages,
              "Packages whose neighborhood to export; all when omitted")
        ->type_name("PACKAGE");
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
    add_command<Shell>(app, invocation,
                       "Answer commands read one per line from standard input, loading the stores "
                       "once");
    add_command<Rebuild>(app, invocation, "Rebuild the store from scratch");
    add_command<Refresh>(app, invocation,
                         "Bring the store up to date if its inputs changed, printing nothing");
    add_command<Check>(app, invocation, "Diff the store against a fresh build");
    add_field(
        add_dynamic_deps(add_command<Affected>(
            app, invocation,
            "What a set of merges can affect, as JSON, for portage's neighborhood completion")),
        invocation, "--request", &Affected::request,
        "JSON request file (default -: standard input)")
        ->type_name("FILE");
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
                                         const std::filesystem::path& path,
                                         std::span<const std::string> cps) {
    std::vector<std::string> argv{builder_program(invocation), std::string{mode}, "--store",
                                  path.string()};
    add_roots(argv, invocation);
    argv.insert(argv.end(), cps.begin(), cps.end());
    return argv;
}

std::vector<std::string> emerge_command(const Invocation& invocation,
                                        const EmergeRequest& request) {
    return emerge_command(invocation, pretend_arguments(request));
}

std::vector<std::string> emerge_command(const Invocation& invocation,
                                        std::span<const std::string> arguments) {
    std::vector<std::string> argv{invocation.emerge.value_or("emerge"), "--root",
                                  invocation.root.string()};
    if (invocation.config_root) {
        argv.insert(argv.end(), {"--config-root", invocation.config_root->string()});
    }
    if (invocation.eprefix) {
        argv.insert(argv.end(), {"--prefix", invocation.eprefix->string()});
    }
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return argv;
}

std::vector<std::string> emerge_options_command(const Invocation& invocation,
                                                const std::filesystem::path& output) {
    std::vector<std::string> argv{builder_program(invocation), "--emerge-options", "--output",
                                  output.string()};
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

namespace {

Exit dispatch(Session& session, const Invocation& invocation, std::ostream& out,
              std::ostream& err) {
    return std::visit(
        [&](const auto& command) { return execute(command, session, invocation, out, err); },
        invocation.command);
}

// The option choosing the stores that a shell line set otherwise than the session, if any.
std::optional<std::string_view> changed_stores(const Invocation& session, const Invocation& line) {
    if (line.root != session.root) {
        return "--root";
    }
    if (line.config_root != session.config_root) {
        return "--config-root";
    }
    if (line.eprefix != session.eprefix) {
        return "--eprefix";
    }
    if (line.store != session.store) {
        return "--store";
    }
    if (line.builder != session.builder) {
        return "--builder";
    }
    if (line.no_refresh != session.no_refresh) {
        return "--no-refresh";
    }
    return std::nullopt;
}

std::string_view trimmed(std::string_view text) {
    constexpr std::string_view space = " \t\r";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

} // namespace

namespace {

LineResult run_line(Session& session, const Invocation& invocation, std::string_view text,
                    std::ostream& out, std::ostream& err, Context context) {
    const std::string_view where = context == Context::shell ? "shell" : "interface";
    const auto line = trimmed(text);
    if (line.empty() || line.starts_with('#')) {
        return {};
    }
    if (line == "quit" || line == "exit" || (context == Context::interface && line == "q")) {
        return {.quit = true};
    }
    // Each line starts from the session's own options and facts, and asks nothing.
    auto command = invocation;
    command.command = std::monostate{};
    command.ask = false;
    CLI::App app{"", "egraph"};
    configure(app, command);
    if (line == "help") {
        out << app.help();
        return {};
    }
    const auto usage = [&](const auto&... message) {
        ((err << "egraph: " << where << ": ") << ... << message) << '\n';
        return LineResult{.quit = false, .exit = Exit::usage};
    };
    // CLI11 only says a subcommand is required.
    if (const auto word = line.substr(0, line.find_first_of(" \t"));
        !word.starts_with('-') && app.get_subcommand_no_throw(std::string{word}) == nullptr) {
        return usage(word, ": no such command (help lists them)");
    }
    try {
        app.parse(std::string{line}, false);
    } catch (const CLI::ParseError& e) {
        // --help arrives here too, with exit code 0.
        return {.quit = false, .exit = app.exit(e, out, err) == 0 ? Exit::ok : Exit::usage};
    }
    if (const auto option = changed_stores(invocation, command)) {
        return usage(*option, " chooses the stores, which the ", where,
                     " keeps; start another egraph for others");
    }
    if (std::holds_alternative<std::monostate>(command.command) ||
        std::holds_alternative<Shell>(command.command) ||
        std::holds_alternative<Tui>(command.command)) {
        return usage("already in the ", where);
    }
    // emerge would write over the interface's screen.
    if (context == Context::interface && (std::holds_alternative<Update>(command.command) ||
                                          std::holds_alternative<Install>(command.command) ||
                                          std::holds_alternative<Remove>(command.command) ||
                                          std::holds_alternative<Select>(command.command) ||
                                          std::holds_alternative<Deselect>(command.command))) {
        return usage("actions run from the command line or the shell");
    }
    if (context == Context::interface) {
        // The interface lays the fields out itself.
        command.layout = Layout::lines;
    }
    return {.quit = false, .exit = dispatch(session, command, out, err)};
}

Exit run_shell(Session& session, const Invocation& invocation, std::istream& in, std::ostream& out,
               std::ostream& err, bool prompt) {
    auto status = Exit::ok;
    for (std::string line;;) {
        if (prompt) {
            out << "egraph> " << std::flush;
        }
        if (!std::getline(in, line)) {
            if (prompt) {
                out << '\n';
            }
            break;
        }
        const auto result = run_line(session, invocation, line, out, err, Context::shell);
        if (result.quit) {
            break;
        }
        if (!trimmed(line).empty() && !trimmed(line).starts_with('#')) {
            status = result.exit;
        }
    }
    return status;
}

} // namespace

Exit shell(const Invocation& invocation, std::istream& in, std::ostream& out, std::ostream& err,
           bool prompt) {
    Session session{invocation, err};
    return run_shell(session, invocation, in, out, err, prompt);
}

bool answered_yes(std::istream& in, std::ostream& out, std::string_view question) {
    out << question << " [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(in, answer)) {
        out << '\n';
        return false;
    }
    const auto word = trimmed(answer);
    return word == "y" || word == "Y" || word == "yes" || word == "Yes" || word == "YES";
}

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    Session session{invocation, err};
    auto asking = invocation;
    asking.ask = invocation.terminal && invocation.input_terminal;
    return dispatch(session, asking, out, err);
}

} // namespace egraph
