#include "cli.hpp"

#include "action.hpp"
#include "affected.hpp"

#include "atom.hpp"
#include "blockers.hpp"
#include "build_info.hpp"
#include "check.hpp"
#include "depclean.hpp"
#include "elog.hpp"
#include "emerge.hpp"
#include "evaluated.hpp"
#include "exec.hpp"
#include "freshness.hpp"
#include "graph.hpp"
#include "history.hpp"
#include "human.hpp"
#include "json.hpp"
#include "keep_going.hpp"
#include "log_read.hpp"
#include "merge_wait.hpp"
#include "notices.hpp"
#include "observe.hpp"
#include "os.hpp"
#include "package_use.hpp"
#include "pool.hpp"
#include "pressure.hpp"
#include "remove.hpp"
#include "replace.hpp"
#include "request.hpp"
#include "resume.hpp"
#include "run_log.hpp"
#include "run_state.hpp"
#include "schedule.hpp"
#include "selection.hpp"
#include "session.hpp"
#include "status.hpp"
#include "steve.hpp"
#include "store.hpp"
#include "tui.hpp"
#include "visibility.hpp"
#include "watch.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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
#include <ranges>
#include <sstream>
#include <string_view>
#include <type_traits>
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

// --resume-list, filling the path field() returns.
template <class Field> void add_resume_list(CLI::App* sub, Field field) {
    sub->add_option_function<std::string>(
           "--resume-list", [field](const std::string& path) { field() = path; },
           "Also write the plan to FILE as emerge's resume list (mtimedb's resume entry, JSON)")
        ->type_name("FILE");
}

// --requests, filling the path field() returns.
template <class Field> void add_requests(CLI::App* sub, Field field) {
    sub->add_option_function<std::string>(
           "--requests", [field](const std::string& path) { field() = path; },
           "Also write the plan to FILE as the requests egraph-build --worker takes, a JSON "
           "object a line")
        ->type_name("FILE");
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
        "In merge order, each with the places and kinds of the merges it waits for");
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
    if constexpr (!std::is_same_v<C, Update>) {
        sub->add_flag_callback(
            "--verify", [updates] { updates().verify = true; },
            "Also ask emerge --pretend, and show where its merge list differs");
    }
    if constexpr (std::is_same_v<C, Updates>) {
        add_resume_list(sub, [updates]() -> auto& { return updates().resume_list; });
        add_requests(sub, [updates]() -> auto& { return updates().requests; });
    }
}

// plan's options, for C: PlanCommand or a command extending it.
template <class C> void add_plan_options(CLI::App* sub, Invocation& invocation) {
    const auto plan = [&invocation]() -> PlanCommand& { return std::get<C>(invocation.command); };
    sub->add_option_function<std::vector<std::string>>(
           "targets", [plan](const std::vector<std::string>& targets) { plan().targets = targets; },
           "Atoms and sets (@world, @selected, @system, @profile, @installed), as emerge's")
        ->type_name("ATOM")
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
        "In merge order, each with the places and kinds of the merges it waits for");
    if constexpr (std::is_same_v<C, PlanCommand>) {
        sub->add_flag_callback(
            "--verify", [plan] { plan().verify = true; },
            "Also ask emerge --pretend, and show where its merge list differs");
        add_resume_list(sub, [plan]() -> auto& { return plan().resume_list; });
        add_requests(sub, [plan]() -> auto& { return plan().requests; });
    }
}

// An action's --yes.
template <class C>
void add_yes(
    CLI::App* sub, Invocation& invocation,
    std::string description =
        "Run emerge without asking, as scripts must where there is no terminal to ask on") {
    sub->add_flag_callback(
        "-y,--yes", [&invocation] { std::get<C>(invocation.command).yes = true; },
        std::move(description));
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
    if (const auto error = refresh_store(invocation, "--full", store_path(invocation), err)) {
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
    if (const auto index = session.repository(); !index) {
        err << "egraph: " << index.error() << '\n';
        return Exit::failure;
    }
    return Exit::ok;
}

// A watcher on each directory, or on the nearest one above that exists, for an input not made
// yet; one that cannot be read goes unwatched, as its inputs cannot change for this user either.
std::expected<os::Watcher, std::string>
watch_all(std::span<const std::filesystem::path> directories, std::size_t& unreadable) {
    auto watcher = os::Watcher::open();
    if (!watcher) {
        return std::unexpected("inotify: " + watcher.error().message());
    }
    unreadable = 0;
    for (const auto& directory : directories) {
        for (auto at = directory;; at = at.parent_path()) {
            const auto added = watcher->add(at);
            if (added) {
                break;
            }
            if (added.error() == std::errc::no_space_on_device) {
                return std::unexpected(std::format(
                    "inotify: more directories ({}) than fs.inotify.max_user_watches allows",
                    directories.size()));
            }
            if (added.error() == std::errc::permission_denied) {
                ++unreadable;
                break;
            }
            if (!at.has_relative_path()) {
                break;
            }
        }
    }
    return std::move(*watcher);
}

std::optional<std::string> refresh_status(Session& session, const Invocation& invocation);

Exit execute(const Watch&, Session& session, const Invocation& invocation, std::ostream&,
             std::ostream& err) {
    if (const auto caught = os::catch_stop_signals(); !caught) {
        err << "egraph: watch: " << caught.error().message() << '\n';
        return Exit::failure;
    }
    using Clock = std::chrono::steady_clock;
    bool first = true;
    std::size_t unreadable = 0;
    std::size_t reported = 0;
    const auto refresh = [&]() -> std::expected<std::vector<std::filesystem::path>, std::string> {
        const auto started = Clock::now();
        session.reload();
        const auto stores = session.stores();
        if (!stores) {
            return std::unexpected(stores.error());
        }
        const auto index = session.repository();
        if (!index) {
            return std::unexpected(index.error());
        }
        const std::array<std::span<const Input>, 3> layers{
            stores->get().installed.inputs, stores->get().evaluated.inputs, index->get().inputs};
        auto directories = watch_directories(layers);
        if (const auto error = refresh_status(session, invocation)) {
            err << "egraph: watch: " << *error << '\n';
        }
        // Nothing reads them until the next refresh.
        session.reload();
        const auto took = std::chrono::duration<double>(Clock::now() - started).count();
        if (first) {
            err << std::format("egraph: watch: watching {} directories for {}\n",
                               directories.size(), session.used().string());
            first = false;
        } else {
            err << std::format("egraph: watch: refreshed in {:.1f} s\n", took);
        }
        return directories;
    };
    const auto watch = [&](const std::vector<std::filesystem::path>& directories) {
        auto watcher = watch_all(directories, unreadable);
        if (watcher && unreadable != reported) {
            err << std::format("egraph: watch: {} directories cannot be read, so go unwatched\n",
                               unreadable);
            reported = unreadable;
        }
        return watcher;
    };
    // Asked once after each refresh, the last to load the stores before a wait.
    const auto stale = [&invocation] {
        std::filesystem::path used;
        const bool went_stale =
            !current_stores(invocation, used) || !current_repository(invocation, used);
        os::release_memory();
        return went_stale;
    };
    const auto kept = keep_fresh(refresh, watch, stale, [] { return Clock::now(); }, err);
    if (!kept) {
        err << "egraph: watch: " << kept.error() << '\n';
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
    return load_path(files.path()).transform_error([&invocation](const StoreError& e) {
        return built_store_error(builder_program(invocation), e);
    });
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
            return tui::CheckResult{
                std::unexpected(built_store_error(builder_program(invocation), loaded.error()))};
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
    return [build = background_build(invocation, "--full", path), path,
            builder = builder_program(
                invocation)]() mutable -> std::optional<std::expected<Stores, std::string>> {
        auto ended = build();
        if (!ended) {
            return std::nullopt;
        }
        if (*ended) {
            return std::expected<Stores, std::string>{std::unexpected(std::move(**ended))};
        }
        return load_stores(path).transform_error(
            [&builder](const StoreError& e) { return built_store_error(builder, e); });
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
    return [build = background_build(invocation, "--incremental", used), used, &session,
            builder = builder_program(invocation)]() mutable -> std::optional<tui::RefreshResult> {
        auto ended = build();
        if (!ended) {
            return std::nullopt;
        }
        if (*ended) {
            return tui::RefreshResult{std::unexpected(std::move(**ended))};
        }
        auto loaded = load_stores(used);
        if (!loaded) {
            return tui::RefreshResult{std::unexpected(built_store_error(builder, loaded.error()))};
        }
        return share(session, std::move(*loaded), used);
    };
}

// The repository index in the background, as a session opens it: a current one, else the user's
// after a build (or as it is, with --no-refresh). The session answers from it too.
Job<tui::IndexResult> background_index(const Invocation& invocation, Session& session) {
    if (auto loaded = session.loaded_repository()) {
        return ready(tui::IndexResult{std::move(loaded)});
    }
    std::filesystem::path used;
    if (auto current = current_repository(invocation, used)) {
        return ready(tui::IndexResult{session.adopt_repository(std::move(*current))});
    }
    const auto builder = builder_program(invocation);
    const auto adopt = [&session, builder](std::expected<RepositoryIndex, StoreError> loaded,
                                           bool built) -> tui::IndexResult {
        if (!loaded) {
            return std::unexpected(built ? built_store_error(builder, loaded.error())
                                         : stored_store_error(loaded.error()));
        }
        return session.adopt_repository(std::move(*loaded));
    };
    if (invocation.no_refresh) {
        return ready(adopt(load_repository(repository_index_path(used)), false));
    }
    return [build = background_build(invocation, "--repository", used), used,
            adopt]() mutable -> std::optional<tui::IndexResult> {
        auto ended = build();
        if (!ended) {
            return std::nullopt;
        }
        if (*ended) {
            return tui::IndexResult{std::unexpected(std::move(**ended))};
        }
        return adopt(load_repository(repository_index_path(used)), true);
    };
}

// The interface's action as this session's command shows it, stopped where it would ask.
tui::Preview preview_action(Session& session, const Invocation& invocation, std::ostream& err,
                            const tui::Action& action) {
    auto command = invocation;
    command.command = std::monostate{};
    CLI::App app{"", "egraph"};
    configure(app, command);
    auto words = tui::action_arguments(action);
    std::ranges::reverse(words);
    try {
        app.parse(words);
    } catch (const CLI::ParseError& e) {
        return {.ready = false, .out = {}, .err = e.what()};
    }
    // Set after parsing, which reads the environment's EGRAPH_LAYOUT.
    command.ask = false;
    command.preview = true;
    command.layout = Layout::human;
    command.color = ColorMode::never;
    std::ostringstream shown;
    std::ostringstream problems;
    session.warn_to(problems);
    const auto exit = dispatch(session, command, shown, problems);
    session.warn_to(err);
    return {.ready = exit == Exit::previewed, .out = shown.str(), .err = problems.str()};
}

// The last bytes of a file, enough for a log's last lines.
std::optional<std::string> file_tail(const std::filesystem::path& path) {
    constexpr std::streamoff most = std::streamoff{64} * 1024;
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    in.seekg(0, std::ios::end);
    in.seekg(std::max<std::streamoff>(0, static_cast<std::streamoff>(in.tellg()) - most));
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// How many of a log's last lines a failed run shows.
constexpr std::size_t tail_lines = 20;

// The failures exec recorded for the run started at since, each with its log's last lines; none
// when the record is older, as when the run stopped before it began.
std::vector<tui::RunFailure> recorded_failures(const std::filesystem::path& state,
                                               std::filesystem::file_time_type since) {
    std::error_code error;
    if (std::filesystem::last_write_time(state, error) < since || error) {
        return {};
    }
    const auto text = file_tail(state);
    const auto recorded = parse_run_state(text.value_or(""));
    if (!recorded) {
        return {};
    }
    std::vector<tui::RunFailure> failures;
    for (const auto& failure : recorded->failed) {
        auto tail = failure.log.empty() || failure.log.ends_with(".gz")
                        ? std::vector<std::string>{}
                        : tui::plain_tail(file_tail(failure.log).value_or(""), tail_lines);
        failures.push_back({.cpv = failure.cpv, .log = failure.log, .tail = std::move(tail)});
    }
    return failures;
}

// A run of its own an object owns, left running when the object goes.
struct Detached {
    explicit Detached(os::Child started) : child{std::move(started)} {}
    Detached(const Detached&) = delete;
    Detached& operator=(const Detached&) = delete;
    Detached(Detached&&) noexcept = default;
    Detached& operator=(Detached&&) noexcept = default;
    ~Detached() { child.detach(); }
    os::Child child;
};

// The interface's action carried out by an egraph of its own, its output to a file beside exec's
// record: the job ends with the run, and the run outlives the job, so quitting the interface
// leaves it to finish.
Job<tui::RunResult> start_action(const Invocation& invocation, const std::filesystem::path& eroot,
                                 const tui::Action& action) {
    const auto program = os::executable();
    std::vector<std::string> argv{program.empty() ? std::string{"egraph"} : program.string()};
    std::ranges::move(egraph_options(invocation), std::back_inserter(argv));
    argv.insert(argv.end(), {"--layout", "human", "--color", "never"});
    std::ranges::move(tui::action_arguments(action), std::back_inserter(argv));
    argv.emplace_back("--yes");
    if (!invocation.dynamic_deps) {
        argv.insert(argv.end(), {"--dynamic-deps", "n"});
    }
    const auto state = run_state_path(eroot);
    const auto output = state.parent_path() / "interface.log";
    std::error_code ignored;
    std::filesystem::create_directories(output.parent_path(), ignored);
    const auto since = std::filesystem::file_time_type::clock::now();
    auto child = os::start(argv, output);
    if (!child) {
        return ready(tui::RunResult{.error = child.error().message});
    }
    return [running = Detached{std::move(*child)}, state, output, since,
            merges = action.kind !=
                     tui::Action::Kind::remove]() mutable -> std::optional<tui::RunResult> {
        auto ended = running.child.poll();
        if (!ended) {
            return std::nullopt;
        }
        if (!*ended) {
            return tui::RunResult{.error = std::move(ended->error().message)};
        }
        tui::RunResult outcome{
            .error = {}, .status = **ended, .failures = {}, .output = output.string(), .tail = {}};
        if (outcome.status != 0) {
            if (merges) {
                outcome.failures = recorded_failures(state, since);
            }
            outcome.tail = tui::plain_tail(file_tail(output).value_or(""), tail_lines);
        }
        return outcome;
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
        err << "egraph: " << stored_store_error(stored.error()) << '\n';
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

// The replace-slots list a plan for command reads.
std::expected<std::vector<Atom>, Exit> replace_list(const Invocation& invocation,
                                                    std::string_view command, std::ostream& err) {
    auto found = read_replace_slots(
        invocation.replace_slots.value_or(replace_slots_path(config_root(invocation))));
    if (!found) {
        err << "egraph: " << command << ": " << found.error() << '\n';
        return std::unexpected(Exit::failure);
    }
    return std::move(*found);
}

// The slots of plan's replacements that hold the running kernel's sources, as egraph-build
// reads their CONTENTS: every one when it cannot tell.
std::vector<std::uint32_t> running_kernel_slots(const Invocation& invocation, const Store& store,
                                                const Plan& plan, std::string_view command,
                                                std::ostream& err) {
    const auto replaced = replaced_slots(plan);
    const auto release = kernel_release();
    const auto running = release ? kernel_sources(invocation.root, *release) : std::nullopt;
    if (replaced.empty() || !running) {
        return {};
    }
    std::vector<std::string> cpvs;
    cpvs.reserve(replaced.size());
    for (const auto id : replaced) {
        cpvs.emplace_back(store.string(store.packages.at(id).cpv));
    }
    const auto output = std::filesystem::path{scratch_store()}.replace_extension(".json");
    const auto ran = output_of(kernel_sources_command(invocation, output, cpvs));
    std::ifstream in{output};
    std::ostringstream text;
    text << in.rdbuf();
    in.close();
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    auto sources = ran ? parse_kernel_sources(text.str()) : std::unexpected(ran.error());
    if (!sources) {
        const auto last = sources.error().rfind('\n');
        err << "egraph: " << command << ": keeping every old slot, as which hold the running "
            << "kernel's sources is unknown: "
            << (last == std::string::npos ? sources.error() : sources.error().substr(last + 1))
            << '\n';
        return replaced;
    }
    return running_kernel_owners(store, replaced, *sources, *running);
}

// plan_updates for targets, without replacing the slots that hold the running kernel's sources.
Plan plan_keeping_kernel(const Invocation& invocation, const Store& store,
                         const Evaluated& evaluated, UseRebuilds rebuilds, Targets& targets,
                         std::string_view command, std::ostream& err) {
    auto plan = plan_updates(store, evaluated, rebuilds, targets);
    if (!targets.running_root) {
        return plan;
    }
    targets.kept_slots = running_kernel_slots(invocation, store, plan, command, err);
    return targets.kept_slots.empty() ? plan : plan_updates(store, evaluated, rebuilds, targets);
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

Exit execute(const Search& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto stores = session.stores();
    if (!stores) {
        return fail(err, stores.error());
    }
    const auto index = session.repository();
    if (!index) {
        return fail(err, index.error());
    }
    const VersionMasks masks{*index};
    const auto lines = search_lines(stores->get().installed, stores->get().evaluated, *index, masks,
                                    command.keys, command.options);
    if (!lines) {
        return fail(err, lines.error());
    }
    if (const auto style = output(invocation); style.human) {
        human_search(out, *lines, command.keys, style.theme);
    } else {
        write_lines(out, *lines);
    }
    return Exit::ok;
}

Exit execute(const Versions& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto index = session.repository();
    if (!index) {
        return fail(err, index.error());
    }
    const VersionMasks masking{*index};
    const auto lines = version_lines(*index, masking, command.packages);
    if (!lines) {
        return fail(err, lines.error());
    }
    if (const auto style = output(invocation); style.human) {
        human_versions(out, *lines, style.theme);
    } else {
        write_lines(out, *lines);
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
    const auto path = package_use_path(config_root(invocation));
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
    // emerge's arguments as the request named them; none for updates.
    std::vector<Argument> arguments;
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
    if (command.world) {
        // -uD goes where the versions it moves to lead, which depclean's kept set does not.
        const auto world = parse_request(*store, evaluated, std::array{std::string("@world")});
        if (!world) {
            err << "egraph: updates: " << world.error() << '\n';
            return std::unexpected(Exit::failure);
        }
        targets.reach = request_reach(*store, evaluated, *world);
        for (std::size_t id = 0; id < targets.scope.size(); ++id) {
            targets.scope.at(id) = targets.scope.at(id) || targets.reach.at(id);
        }
    }
    targets.running_root = running_root(invocation);
    targets.dynamic_deps = invocation.dynamic_deps;
    auto replace = replace_list(invocation, "updates", err);
    if (!replace) {
        return std::unexpected(replace.error());
    }
    targets.replace_slots = std::move(*replace);
    Shown shown{.plan = plan_keeping_kernel(invocation, *store, evaluated, command.rebuilds,
                                            targets, "updates", err),
                .request = {.targets = {command.world ? "@world" : "@installed"},
                            .update = true,
                            .deep = command.deep,
                            .rebuilds = command.rebuilds,
                            .dynamic_deps = invocation.dynamic_deps},
                .store = *store,
                .evaluated = evaluated,
                .arguments = {}};
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

// Writes the plan to path as emerge's resume list, but for one emerge would refuse.
Exit write_resume_list(Exit status, const std::optional<std::filesystem::path>& path,
                       std::string_view name, const Shown& shown, bool oneshot, std::ostream& err) {
    if (!path) {
        return status;
    }
    const auto& [plan, request, store, evaluated, arguments] = shown;
    if (plan.refused()) {
        err << "egraph: " << name << ": no resume list, as emerge would refuse the plan\n";
        return status;
    }
    std::ofstream file{*path};
    file << resume_entry(evaluated, plan, store.get().meta.eroot, request, oneshot, arguments)
         << '\n';
    if (!file.flush()) {
        return fail(err, std::format("{}: {}: cannot write", name, path->string()));
    }
    return status;
}

// Writes the plan to path as the worker's requests, but for one emerge would refuse.
Exit write_requests(Exit status, const std::optional<std::filesystem::path>& path,
                    std::string_view name, const Shown& shown, bool oneshot, std::ostream& err) {
    if (!path) {
        return status;
    }
    const auto& [plan, request, store, evaluated, arguments] = shown;
    if (plan.refused()) {
        err << "egraph: " << name << ": no requests, as emerge would refuse the plan\n";
        return status;
    }
    std::ofstream file{*path};
    for (const auto& step : run_steps(store, evaluated, plan, arguments, oneshot)) {
        file << worker_request(store, evaluated, plan, step) << '\n';
    }
    if (!file.flush()) {
        return fail(err, std::format("{}: {}: cannot write", name, path->string()));
    }
    return status;
}

Exit execute(const Updates& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    const auto shown = show_updates(command, session, invocation, out, err);
    if (!shown) {
        return shown.error();
    }
    const auto& [plan, request, store, evaluated, arguments] = *shown;
    auto status =
        write_resume_list(finish(command.verify ? verify(invocation, Updates::name, request, store,
                                                         evaluated, plan, out, err)
                                                : Exit::ok,
                                 plan),
                          command.resume_list, Updates::name, *shown, true, err);
    status = write_requests(status, command.requests, Updates::name, *shown, true, err);
    return offer_use_changes(
        status, plan, store, evaluated, session, invocation, out, err,
        [&](const Invocation& again) { return execute(command, session, again, out, err); });
}

// The updates planned on the session's stores, now, and written to the status file at path; why
// not, when that failed.
std::optional<std::string> write_status(Session& session, const Invocation& invocation,
                                        const StatusStores& now,
                                        const std::filesystem::path& path) {
    auto lines_invocation = invocation;
    lines_invocation.layout = Layout::lines;
    Updates command;
    command.rebuilds = UseRebuilds::all;
    command.held = true;
    command.world = true;
    command.deep = true;
    std::ostringstream out;
    std::ostringstream err;
    const auto shown = show_updates(command, session, lines_invocation, out, err);
    if (!shown) {
        auto message = std::move(err).str();
        while (message.ends_with('\n')) {
            message.pop_back();
        }
        return std::format("no plan for the status file: {}", message);
    }
    Status status{.written =
                      std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
                  .stores = now,
                  .counts = plan_counts(shown->plan),
                  .repositories = {},
                  .lines = {}};
    const auto index = session.repository();
    if (!index) {
        return index.error();
    }
    const auto& repository_index = index->get();
    for (const auto& repository : repository_index.repositories) {
        const auto location = repository_index.string(repository.location);
        status.repositories.push_back(
            {.name = std::string{repository_index.string(repository.name)},
             .synced = repository_synced(std::string{location})});
    }
    for (const auto line : std::views::split(std::move(out).str(), '\n')) {
        if (!line.empty()) {
            status.lines.emplace_back(line.begin(), line.end());
        }
    }
    if (const auto written = os::replace_with_text(path, status_json(status)); !written) {
        return std::format("cannot write {}: {}", path.string(), written.error().message());
    }
    return std::nullopt;
}

// The build times of the session's stores, loading (and refreshing) them.
std::expected<StatusStores, std::string> session_build_times(Session& session) {
    const auto stores = session.stores();
    if (!stores) {
        return std::unexpected(stores.error());
    }
    const auto index = session.repository();
    if (!index) {
        return std::unexpected(index.error());
    }
    return StatusStores{.installed = {.path = session.used().string(),
                                      .built = stores->get().installed.meta.build_time_ns},
                        .evaluated = {.path = evaluated_store_path(session.used()).string(),
                                      .built = stores->get().evaluated.meta.build_time_ns},
                        .repository = {.path = session.repository_used().string(),
                                       .built = index->get().meta.build_time_ns}};
}

// The status file at path, or why it cannot be read.
std::expected<Status, std::string> read_status(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error().message);
    }
    std::string text(bytes->size(), '\0');
    std::ranges::transform(*bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return parse_status(text).transform_error(
        [&path](const std::string& error) { return std::format("{}: {}", path.string(), error); });
}

// With the session's stores just refreshed, the status file beside them written anew when the
// settings ask for it then; why not, when that failed. Skipped where this user cannot write.
std::optional<std::string> refresh_status(Session& session, const Invocation& invocation) {
    const auto path = status_path(session.used());
    if (!os::can_create(path)) {
        return std::nullopt;
    }
    const auto settings = read_settings(settings_path(config_root(invocation)));
    if (!settings) {
        return settings.error() + ", so no plan is made";
    }
    const auto now = session_build_times(session);
    if (!now) {
        return now.error();
    }
    const auto old = read_status(path);
    if (!status_due(settings->plan, old ? std::optional{old->stores} : std::nullopt, *now)) {
        return std::nullopt;
    }
    return write_status(session, invocation, *now, path);
}

// The request the targets name, the cps only the repositories know evaluated first.
// What needs the user once emerge has run, through egraph-build.
std::expected<Notices, std::string> read_notices(const Invocation& invocation) {
    const auto output = std::filesystem::path{scratch_store()}.replace_extension(".notices");
    const auto ran = output_of(notices_command(invocation, output));
    std::ostringstream text;
    if (std::ifstream in{output}; in) {
        text << in.rdbuf();
    }
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    if (!ran) {
        return std::unexpected(ran.error());
    }
    return parse_notices(text.str());
}

// The sets a request may name that egraph-build loads as emerge does: @preserved-rebuild.
std::expected<Sets, std::string> given_sets(std::span<const std::string> targets,
                                            const Invocation& invocation) {
    constexpr std::string_view name = "preserved-rebuild";
    if (!std::ranges::contains(targets, std::format("@{}", name))) {
        return Sets{};
    }
    const auto notices = read_notices(invocation);
    if (!notices) {
        return std::unexpected(std::format("@{}: {}", name, notices.error()));
    }
    if (!notices->preserved) {
        return std::unexpected(std::format(
            "@{}: the preserved libraries' registry cannot be read; run egraph as root", name));
    }
    if (!notices->rebuild) {
        return std::unexpected(
            std::format("@{}: what uses the preserved libraries cannot be found", name));
    }
    return Sets{{std::string(name), *notices->rebuild}};
}

std::expected<Request, std::string> resolve_request(const PlanCommand& command, Session& session,
                                                    const Invocation& invocation) {
    const auto given = given_sets(command.targets, invocation);
    if (!given) {
        return std::unexpected(given.error());
    }
    for (bool evaluated = false;; evaluated = true) {
        const auto stores = session.stores();
        if (!stores) {
            return std::unexpected(stores.error());
        }
        const auto store = session.dependencies(invocation.dynamic_deps);
        if (!store) {
            return std::unexpected(store.error());
        }
        auto request = parse_request(*store, stores->get().evaluated, command.targets, *given);
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
    targets.dynamic_deps = invocation.dynamic_deps;
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
    auto replace = replace_list(invocation, name, err);
    if (!replace) {
        return std::unexpected(replace.error());
    }
    targets.replace_slots = std::move(*replace);
    Shown shown{.plan = plan_keeping_kernel(invocation, *store, evaluated, command.rebuilds,
                                            targets, name, err),
                .request = {.targets = command.targets,
                            .update = command.update,
                            .deep = command.deep,
                            .noreplace = command.noreplace,
                            .rebuilds = command.rebuilds,
                            .dynamic_deps = invocation.dynamic_deps},
                .store = *store,
                .evaluated = evaluated,
                .arguments = request->arguments};
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
    const auto& [plan, request, store, evaluated, arguments] = *shown;
    auto status =
        write_resume_list(finish(command.verify ? verify(invocation, PlanCommand::name, request,
                                                         store, evaluated, plan, out, err)
                                                : Exit::ok,
                                 plan),
                          command.resume_list, PlanCommand::name, *shown, false, err);
    status = write_requests(status, command.requests, PlanCommand::name, *shown, false, err);
    return offer_use_changes(
        status, plan, store, evaluated, session, invocation, out, err,
        [&](const Invocation& again) { return execute(command, session, again, out, err); });
}

// What emerge runs under, through egraph-build.
std::expected<RunSettings, std::string> run_settings(const Invocation& invocation) {
    const auto output = std::filesystem::path{scratch_store()}.replace_extension(".options");
    const auto ran = output_of(emerge_options_command(invocation, output));
    std::ostringstream text;
    if (std::ifstream in{output}; in) {
        text << in.rdbuf();
    }
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    if (!ran) {
        return std::unexpected(ran.error());
    }
    return parse_run_settings(text.str());
}

// A log's size now, nothing for one that is not there yet.
std::uintmax_t log_size(const std::optional<std::string>& path) {
    std::error_code error;
    const auto size = path ? std::filesystem::file_size(*path, error) : 0;
    return error ? 0 : size;
}

// What a log holds past offset: all of it once it has been rotated to less.
std::string appended(const std::string& path, std::uintmax_t offset) {
    std::ostringstream text;
    if (std::ifstream in{path}; in) {
        text << in.rdbuf();
    }
    auto all = std::move(text).str();
    return offset <= all.size() ? all.substr(offset) : all;
}

void show_notices(const Notices& notices, const Invocation& invocation, std::ostream& out) {
    const auto lines = notice_lines(notices);
    if (const auto style = output(invocation); style.human) {
        human_notices(out, lines, style.theme);
    } else {
        write_lines(out, lines);
    }
}

// The notices, set apart from what a run printed before them; nullopt, said on err, when they
// could not be read.
std::optional<Notices> show_notices_after(std::string_view name, const Invocation& invocation,
                                          std::ostream& out, std::ostream& err) {
    auto notices = read_notices(invocation);
    if (!notices) {
        err << "egraph: " << name << ": the notices could not be read: " << notices.error() << '\n';
        return std::nullopt;
    }
    if (output(invocation).human && !notice_lines(*notices).empty()) {
        out << '\n';
    }
    show_notices(*notices, invocation, out);
    return std::move(*notices);
}

Exit execute(const Install& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err);

// Seconds since the epoch, as logged events take them.
double epoch_seconds() {
    const std::chrono::duration<double> since = std::chrono::system_clock::now().time_since_epoch();
    return since.count();
}

// egraph's log, where the invocation sends it.
log::Log logger_of(const Invocation& invocation, std::ostream& notes) {
    return {log::targets(invocation.log, log::journal_running()),
            invocation.log_file.value_or(log::default_file(invocation.eprefix.value_or(""))),
            notes};
}

// The command line run for a command handing the system to a program, logged as a run.
std::expected<int, std::string> run_handed_over(const HandOver& hand_over,
                                                const Invocation& invocation, std::ostream& notes) {
    auto logger = logger_of(invocation, notes);
    const auto run = log::new_run();
    const auto started = epoch_seconds();
    logger.write(handed_over(run, hand_over, started));
    const auto ran =
        os::run(hand_over.argv).transform_error([](const os::SpawnError& e) { return e.message; });
    logger.write(handed_back(run, hand_over, ran, started, epoch_seconds()));
    return ran;
}

// After emerge has run: the notices, dispatch-conf offered for configuration updates, and the
// rebuild of what uses preserved libraries planned and offered, once.
void follow_up(std::string_view name, bool yes, Session& session, const Invocation& invocation,
               std::ostream& out, std::ostream& err) {
    const auto notices = show_notices_after(name, invocation, out, err);
    if (!notices || yes || !invocation.ask) {
        return;
    }
    if (!notices->config.empty() && answered_yes(std::cin, out, "Run dispatch-conf now?")) {
        out << std::flush;
        const auto ran = os::run(dispatch_conf_command(invocation));
        if (!ran) {
            err << "egraph: " << name << ": " << ran.error().message << '\n';
        } else if (*ran != 0) {
            err << "egraph: " << name << ": dispatch-conf exited with status " << *ran << '\n';
        }
    }
    if (notices->rebuild.value_or(std::vector<std::string>{}).empty() ||
        invocation.rebuild_offered) {
        return;
    }
    out << "\nRebuilding what uses the preserved libraries:\n";
    Install rebuild;
    rebuild.targets = {"@preserved-rebuild"};
    rebuild.oneshot = true;
    auto again = invocation;
    again.rebuild_offered = true;
    std::ignore = execute(rebuild, session, again, out, err);
}

Exit execute(const NoticesCommand&, Session&, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    const auto notices = read_notices(invocation);
    if (!notices) {
        return fail(err, std::format("notices: {}", notices.error()));
    }
    if (notice_lines(*notices).empty()) {
        if (output(invocation).human) {
            out << "Nothing needs attention.\n";
        }
        return Exit::ok;
    }
    show_notices(*notices, invocation, out);
    return Exit::ok;
}

Exit execute(const Sync& command, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    out << std::flush;
    const auto ran = run_handed_over({.command = std::string{Sync::name},
                                      .targets = command.repositories,
                                      .program = "emaint",
                                      .argv = sync_command(invocation, command.repositories)},
                                     invocation, out);
    // The repositories that synced before a failure count too.
    session.reload();
    if (output(invocation).human) {
        out << '\n';
    }
    const auto status =
        execute(static_cast<const Updates&>(command), session, invocation, out, err);
    std::ignore = show_notices_after(Sync::name, invocation, out, err);
    if (!ran) {
        err << "egraph: " << Sync::name << ": " << ran.error() << '\n';
        return Exit::failure;
    }
    if (*ran != 0) {
        err << "egraph: " << Sync::name << ": emaint exited with status " << *ran << '\n';
        return Exit::failure;
    }
    return status;
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
    case Stop::previewed:
        return Exit::previewed;
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

// Carries a confirmed plan out, under the settings emerge runs under: the exit status of what
// ran, or why it could not run or stopped short.
using CarryOut = std::function<std::expected<int, std::string>(const RunSettings&)>;

// argv run with PORTAGE_ELOG_SYSTEM set as the settings want it, egraph showing the summary.
std::vector<std::string> with_elog_system(std::vector<std::string> argv,
                                          const RunSettings& settings) {
    if (settings.elog_system) {
        argv.insert(argv.begin(), {"env", "PORTAGE_ELOG_SYSTEM=" + *settings.elog_system});
    }
    return argv;
}

// An action once verified: asks unless yes, carries the plan out, then shows the elog summary
// and the selection changes and refreshes the stores; with follow, also the notices and what
// to do about them. what names what carried it out.
Exit confirm_and_carry_out(std::string_view name, bool yes, std::string_view question,
                           std::string_view what, bool follow, const CarryOut& carry_out,
                           const Store& store, Session& session, const Invocation& invocation,
                           std::ostream& out, std::ostream& err) {
    // Copied before the session reloads, which takes store with it.
    const auto selected = world_atoms(store);
    const auto settings = run_settings(invocation);
    if (!settings) {
        err << "egraph: " << name << ": EMERGE_DEFAULT_OPTS could not be read: " << settings.error()
            << '\n';
        return Exit::failure;
    }
    if (!yes && !answered_yes(std::cin, out, question)) {
        return Exit::failure;
    }
    const auto logged = log_size(settings->elog_summary);
    out << std::flush;
    const auto ran = carry_out(*settings);
    if (settings->elog_summary) {
        const auto messages =
            elog_lines(parse_elog_summary(appended(*settings->elog_summary, logged)));
        if (const auto style = output(invocation); style.human) {
            human_elog(out, messages, style.theme);
        } else {
            write_lines(out, messages);
        }
    }
    // What was done before a failure counts too.
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
    if (follow) {
        follow_up(name, yes, session, invocation, out, err);
    } else {
        std::ignore = show_notices_after(name, invocation, out, err);
    }
    if (!ran) {
        err << "egraph: " << name << ": " << ran.error() << '\n';
        return Exit::failure;
    }
    if (*ran != 0) {
        err << "egraph: " << name << ": " << what << " exited with status " << *ran << '\n';
        return Exit::failure;
    }
    return Exit::ok;
}

// What emerge runs with once it has carried out a plan, given EMERGE_DEFAULT_OPTS' execution
// options; nothing when empty.
using Cleanup = std::function<std::vector<std::string>(const std::vector<std::string>& passed)>;

// An action on targets once verified: asks unless yes, runs emerge with arguments(passed),
// passed being EMERGE_DEFAULT_OPTS' execution options, then once it succeeds with cleanup's
// arguments for the cleaned targets, and refreshes the stores after.
template <class Arguments>
Exit confirm_and_run(std::string_view name, bool yes, std::string_view question,
                     const std::vector<std::string>& targets, const Arguments& arguments,
                     const Store& store, Session& session, const Invocation& invocation,
                     std::ostream& out, std::ostream& err, const Cleanup& cleanup = {},
                     const std::vector<std::string>& cleaned = {}) {
    return confirm_and_carry_out(
        name, yes, question, "emerge", true,
        [&](const RunSettings& settings) {
            const auto passed = execution_options(settings.defaults);
            const auto run = [&](const std::vector<std::string>& these,
                                 const std::vector<std::string>& argv) {
                return run_handed_over(
                    {.command = std::string{name},
                     .targets = these,
                     .program = "emerge",
                     .argv = with_elog_system(emerge_command(invocation, argv), settings)},
                    invocation, out);
            };
            auto ran = run(targets, arguments(passed));
            if (!cleanup || !ran || *ran != 0) {
                return ran;
            }
            return run(cleaned, cleanup(passed));
        },
        store, session, invocation, out, err);
}

// An action on a plan: the plan show() shows, verified against emerge --pretend, then carried
// out by carry(shown); act runs the action again once USE changes are written.
template <class Show, class Act, class Carry>
Exit act_on_plan(std::string_view name, bool yes, Session& session, const Invocation& invocation,
                 std::ostream& out, std::ostream& err, const Show& show, const Act& act,
                 const Carry& carry, bool selecting = false) {
    const auto shown = show();
    if (!shown) {
        return shown.error();
    }
    const auto& [plan, request, store, evaluated, arguments] = *shown;
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
                         .can_ask = invocation.ask,
                         .preview = invocation.preview}),
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
    return carry(*shown);
}

// An action merging a plan through emerge: the plan show() shows, verified, confirmed, and
// emerge run on it; act runs the action again once USE changes are written.
template <class Show, class Act>
Exit run_action(std::string_view name, bool oneshot, bool yes, Session& session,
                const Invocation& invocation, std::ostream& out, std::ostream& err,
                const Show& show, const Act& act,
                std::string_view question = "Have emerge merge this plan?",
                bool selecting = false) {
    return act_on_plan(
        name, yes, session, invocation, out, err, show, act,
        [&](const Shown& shown) {
            // emerge's depclean uninstalls the slots the plan replaces, once it has merged.
            std::vector<std::string> replaced;
            for (const auto id : replaced_slots(shown.plan)) {
                replaced.emplace_back(
                    shown.store.get().string(shown.store.get().packages.at(id).cpv));
            }
            Cleanup cleanup;
            if (!replaced.empty()) {
                cleanup = [&](const std::vector<std::string>& passed) {
                    return depclean_arguments(shown.request, replaced, passed);
                };
            }
            std::vector<std::string> cleaned;
            for (const auto& cpv : replaced) {
                cleaned.push_back("=" + cpv);
            }
            return confirm_and_run(
                name, yes, question, shown.request.targets,
                [&](const std::vector<std::string>& passed) {
                    return run_arguments(shown.request, oneshot, passed);
                },
                shown.store, session, invocation, out, err, cleanup, cleaned);
        },
        selecting);
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

// What --trace calls what happened to a step.
std::string_view traced_name(Traced what, bool uninstall) {
    switch (what) {
    case Traced::build_started:
        return "build-start";
    case Traced::built:
        return "built";
    case Traced::build_failed:
        return "build-failed";
    case Traced::merge_started:
        return uninstall ? "uninstall-start" : "merge-start";
    case Traced::merged:
        return uninstall ? "uninstalled" : "merged";
    case Traced::skipped:
        return "skipped";
    case Traced::merge_failed:
        break;
    }
    return uninstall ? "uninstall-failed" : "merge-failed";
}

// A directory removed with this object, whatever is in it.
struct RemovedAtEnd {
    explicit RemovedAtEnd(std::filesystem::path made) : path{std::move(made)} {}
    RemovedAtEnd(const RemovedAtEnd&) = delete;
    RemovedAtEnd& operator=(const RemovedAtEnd&) = delete;
    RemovedAtEnd(RemovedAtEnd&&) = delete;
    RemovedAtEnd& operator=(RemovedAtEnd&&) = delete;
    ~RemovedAtEnd() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

// Runs the steps over a pool of workers, as many builds at once as command or else the settings
// allow, going on after a failure as either says, reporting each event on out: the workers' exit
// status, or why the run stopped short.
std::expected<int, std::string> run_pool(const Invocation& invocation, const RunSettings& settings,
                                         const Exec& command, const Shown& shown,
                                         StepRequests& requests, const std::vector<Step>& steps,
                                         const std::vector<std::string>& names, RunState state,
                                         std::ostream& out) {
    const auto& plan = shown.plan;
    const auto jobs = command.jobs ? command.jobs : jobs_of(execution_options(settings.defaults));
    const bool keep_going =
        command.keep_going.value_or(keep_going_of(execution_options(settings.defaults)));
    std::optional<os::Jobserver> jobserver;
    if (settings.jobserver) {
        auto opened = os::Jobserver::open(*settings.jobserver);
        if (!opened) {
            return std::unexpected(std::format("the jobserver {} could not be opened: {}",
                                               *settings.jobserver, opened.error().message()));
        }
        jobserver = std::move(*opened);
    }
    std::ofstream trace;
    if (command.trace) {
        trace.open(*command.trace, std::ios::trunc);
        if (!trace) {
            return std::unexpected(std::format("{} could not be written", command.trace->string()));
        }
    }
    auto argv = worker_command(invocation);
    if (jobs != 1U) {
        argv.emplace_back("--background");
    }
    // Portage merging itself into the running root: the workers run on a copy of the portage
    // running now, taken before the run, as emerge updates itself.
    std::optional<RemovedAtEnd> portage_copy;
    if (settings.portage_installed && same_root(invocation.root, "/") &&
        merges_cp(plan, shown.evaluated.get(), "sys-apps/portage")) {
        std::random_device random;
        portage_copy.emplace(std::filesystem::path{settings.tmpdir} / "portage" /
                             std::format("._egraph_portage_.{:08x}", random()));
        if (const auto copied = output_of(copy_portage_command(invocation, portage_copy->path));
            !copied) {
            return std::unexpected(
                std::format("portage could not be copied to run from: {}", copied.error()));
        }
        argv.insert(argv.end(), {"--portage-copy", portage_copy->path.string()});
    }
    // Under a make that started us, as emerge holds a token then.
    const auto passed = execution_options(settings.defaults);
    WorkerPool pool{with_elog_system(std::move(argv), settings),
                    std::move(jobserver),
                    os::environment("MAKEFLAGS").has_value(),
                    {.tmpdir = settings.tmpdir, .free_gb = tmpdir_free_gb_of(passed)},
                    out};
    // emerge's merge-wait scope, as its --merge-wait-scope picks it.
    const auto scope =
        merge_wait_scope(command.merge_wait_scope.value_or(merge_wait_scope_of(passed)))
            .value_or(MergeWaitScope::deep);
    Schedule schedule{plan,
                      steps,
                      jobs,
                      {.feature = settings.merge_wait,
                       .alone = merge_wait_steps(shown.store.get(), build_graph(shown.store.get()),
                                                 shown.evaluated.get(), plan, steps, scope),
                       .running_root = same_root(invocation.root, "/")}};
    // Published as emerge's scheduler publishes itself, in egraph's own place.
    std::vector<Observed> observed;
    observed.reserve(steps.size());
    for (const auto& [step, name] : std::views::zip(steps, names)) {
        observed.push_back(
            {.cpv = name,
             .root = shown.store.get().meta.eroot,
             .operation = std::holds_alternative<MergeStep>(step) ? "merge" : "uninstall"});
    }
    Observer observer{os::process_id(), jobs, std::move(observed)};
    StatusFile status{emerge::status_dir(invocation.eprefix.value_or(""), emerge::Publisher::exec) /
                          std::format("exec-{}.json", os::process_id()),
                      out};
    const auto publish = [&](bool tick) {
        const std::chrono::duration<double> now =
            std::chrono::system_clock::now().time_since_epoch();
        observer.update(schedule, now.count());
        status.publish(observer.snapshot(schedule, now.count()), tick);
    };
    publish(true);
    pool.every(std::chrono::seconds{2}, [&] { publish(true); });
    // Logged as egraph's own runs, to the journal or its file.
    std::vector<RunStep> logged;
    logged.reserve(steps.size());
    for (const auto& [step, name] : std::views::zip(steps, names)) {
        logged.push_back({.cpv = name, .uninstall = std::holds_alternative<UninstallStep>(step)});
    }
    RunEvents events{log::new_run(), std::move(logged)};
    auto logger = logger_of(invocation, out);
    const auto now = epoch_seconds;
    logger.write(events.started({.command = std::string{Exec::name},
                                 .targets = shown.request.targets,
                                 .options = request_options(shown.request, command.oneshot),
                                 .jobs = jobs,
                                 .keep_going = keep_going},
                                now()));
    // What exec --resume reads: rewritten as each merge finishes, so a run cut short leaves it.
    const auto state_path = run_state_path(shown.store.get().meta.eroot);
    bool state_told = false;
    const auto record = [&] {
        if (const auto written = os::replace_with_text(state_path, run_state_json(state) + '\n');
            !written && !state_told) {
            out << std::format("egraph: exec: cannot record the run in {}: {}\n",
                               state_path.string(), written.error().message());
            state_told = true;
        }
    };
    record();
    const bool human = output(invocation).human;
    const auto start = std::chrono::steady_clock::now();
    const auto say = [&](std::size_t step, std::string_view what) {
        const auto& name = names.at(step);
        if (human) {
            out << std::format("({} of {}) {}: {}\n", step + 1, steps.size(), name, what);
        } else {
            out << std::format("{}\t{}\t{}\n", step + 1, name, what);
        }
        out << std::flush;
    };
    std::optional<std::string> stuck;
    const auto outcome = run_schedule(
        schedule, pool, requests,
        [&](std::size_t step, const WorkerEvent& event) {
            say(step, describe_event(event));
            if (event.kind == WorkerEvent::Kind::phase) {
                observer.phase(step, event.text);
            }
            if (event.kind == WorkerEvent::Kind::failed || event.kind == WorkerEvent::Kind::error) {
                state.failed.push_back(
                    {.cpv = names.at(step),
                     .log = event.kind == WorkerEvent::Kind::failed ? event.log : std::string{}});
                record();
            }
            publish(false);
            if (const auto logs = events.reported(step, event, now())) {
                logger.write(*logs);
            }
        },
        [&](std::size_t step, Traced what, std::optional<std::size_t> worker) {
            if (worker) {
                if (const auto pid = pool.pid(*worker)) {
                    observer.worker(step, *pid);
                }
            }
            publish(false);
            if (const auto logs = events.traced(step, what, now())) {
                logger.write(*logs);
            }
            if (what == Traced::merged && std::holds_alternative<MergeStep>(steps.at(step))) {
                state.merged.push_back(names.at(step));
                record();
            }
            if (command.trace) {
                const std::chrono::duration<double> since =
                    std::chrono::steady_clock::now() - start;
                trace << std::format("{:.6f}\t{}\t{}\n", since.count(),
                                     traced_name(what, std::holds_alternative<UninstallStep>(
                                                           steps.at(step))),
                                     names.at(step))
                      << std::flush;
            }
        },
        [&](const Schedule& drained) -> std::optional<std::vector<std::size_t>> {
            if (!keep_going) {
                return std::nullopt;
            }
            auto resumed = egraph::keep_going(shown.store.get(), shown.evaluated.get(), plan, steps,
                                              drained.standing());
            if (resumed.stuck) {
                stuck = std::move(resumed.stuck);
                return std::nullopt;
            }
            std::vector<std::size_t> skipped;
            for (const auto& skip : resumed.skipped) {
                const auto why = describe_skip(skip);
                say(skip.step, "skipped, " + why);
                logger.write(events.skipped(skip.step, why, now()));
                skipped.push_back(skip.step);
            }
            observer.resumed(drained, skipped);
            return skipped;
        });
    auto ended = pool.finish();
    if (outcome.stopped) {
        const auto& [step, why] = *outcome.stopped;
        auto message = step ? std::format("{}: {}", names.at(*step), why) : why;
        if (!outcome.also_failed.empty()) {
            message += std::format("; {} more failed", outcome.also_failed.size());
        }
        if (!outcome.skipped.empty()) {
            message += std::format("; {} skipped", outcome.skipped.size());
        }
        if (stuck) {
            message += std::format("; cannot go on without {}, which nothing satisfies", *stuck);
        }
        ended = std::unexpected(std::move(message));
    }
    std::optional<std::string> error;
    if (!ended) {
        error = ended.error();
    } else if (*ended != 0) {
        error = std::format("the workers exited with status {}", *ended);
    }
    logger.write(events.ended(error, now()));
    state.status = error ? RunState::Status::failed : RunState::Status::done;
    record();
    return ended;
}

// Carries out command, a run resumed from one that merged the cpvs merged when it has any.
Exit carry_out(const Exec& command, const std::vector<std::string>& merged, Session& session,
               const Invocation& invocation, std::ostream& out, std::ostream& err) {
    return act_on_plan(
        Exec::name, command.yes, session, invocation, out, err,
        [&] { return show_plan(command, Exec::name, session, invocation, out, err); },
        [&](const Invocation& again) {
            return carry_out(command, merged, session, again, out, err);
        },
        [&](const Shown& shown) {
            const auto& [plan, request, store, evaluated, arguments] = shown;
            auto all = run_steps(store, evaluated, plan, arguments, command.oneshot);
            const auto total = all.size();
            const auto steps = resumed_steps(store, evaluated, plan, std::move(all), merged);
            if (steps.size() < total) {
                out << std::format("egraph: exec: resuming without the {} merged by the last run\n",
                                   total - steps.size() == 1
                                       ? std::string{"merge"}
                                       : std::format("{} merges", total - steps.size()));
            }
            if (steps.empty()) {
                out << "egraph: exec: the last run merged all of it\n";
                return Exit::ok;
            }
            std::vector<std::string> names;
            names.reserve(steps.size());
            for (const auto& step : steps) {
                names.push_back(step_cpv(store, evaluated, plan, step));
            }
            StepRequests requests{store, evaluated, plan, steps};
            return confirm_and_carry_out(
                Exec::name, command.yes, "Have egraph-build --worker merge this plan?",
                "egraph-build --worker", false,
                [&](const RunSettings& settings) {
                    return run_pool(invocation, settings, command, shown, requests, steps, names,
                                    {.arguments = exec_arguments(command), .merged = merged}, out);
                },
                store, session, invocation, out, err);
        });
}

Exit execute(const Exec& command, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    if (!command.resume) {
        return carry_out(command, {}, session, invocation, out, err);
    }
    const auto stores = session.stores();
    if (!stores) {
        return fail(err, stores.error());
    }
    const auto path = run_state_path(stores->get().installed.meta.eroot);
    std::ifstream in{path};
    if (!in) {
        err << std::format("egraph: exec: no run to resume: {} cannot be read\n", path.string());
        return Exit::failure;
    }
    std::ostringstream text;
    text << in.rdbuf();
    const auto state = parse_run_state(text.str());
    if (!state) {
        err << std::format("egraph: exec: {}: {}\n", path.string(), state.error());
        return Exit::failure;
    }
    if (state->status == RunState::Status::done) {
        out << "egraph: exec: the last run finished; nothing to resume\n";
        return Exit::ok;
    }
    const auto resumed = resumed_exec(command, state->arguments);
    if (!resumed) {
        err << std::format("egraph: exec: {}: {}\n", path.string(), resumed.error());
        return Exit::failure;
    }
    return carry_out(*resumed, state->merged, session, invocation, out, err);
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
                                           .can_ask = invocation.ask,
                                           .preview = invocation.preview}),
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
        Remove::name, command.yes, "Have emerge remove these packages?", command.packages,
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
                                                           .can_ask = invocation.ask,
                                                           .preview = invocation.preview}),
                                    Deselect::name, deselect_words, world, style.human, out, err)) {
        return *status;
    }
    return confirm_and_run(
        Deselect::name, command.yes, "Have emerge remove these from @selected?", command.packages,
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
    const auto eprefix = invocation.eprefix.value_or("");
    const auto watch = [emerges = emerge::status_dir(eprefix),
                        runs = emerge::status_dir(eprefix, emerge::Publisher::exec)] {
        auto found = emerge::read_snapshots(emerges);
        std::ranges::move(emerge::read_snapshots(runs, "/proc", emerge::Publisher::exec),
                          std::back_inserter(found));
        std::ranges::sort(found, {}, &emerge::Snapshot::pid);
        return found;
    };
    const auto sample = [] { return pressure::read_sample(); };
    const auto set_steve = [](steve::Setting setting,
                              double value) -> std::expected<void, std::string> {
        return output_of(steve::set_arguments(setting, value)).transform([](const auto&) {});
    };
    // What a command at the prompt prints, and its warnings, go to its answer.
    const auto command = [&session, &invocation, &err](const std::string& line) {
        std::ostringstream out;
        std::ostringstream problems;
        session.warn_to(problems);
        const auto result = run_line(session, invocation, line, out, problems, Context::interface);
        session.warn_to(err);
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
         .load_index = [&invocation, &session] { return background_index(invocation, session); },
         .preview =
             [&session, &invocation, &err](const tui::Action& action) {
                 return preview_action(session, invocation, err, action);
             },
         .run = [&invocation, eroot = (*stores)->installed.meta.eroot](
                    const tui::Action& action) { return start_action(invocation, eroot, action); },
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

Exit execute(const Complete& command, Session&, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    std::vector<std::filesystem::path> paths;
    if (invocation.store) {
        paths.push_back(*invocation.store);
    } else {
        paths.push_back(system_store_path(invocation));
        if (auto user = user_store_path(invocation)) {
            paths.push_back(std::move(*user));
        }
    }
    // The newer of the system's and the user's, whichever the queries last refreshed.
    std::optional<Stores> newest;
    std::filesystem::path chosen;
    std::string error;
    for (const auto& path : paths) {
        auto loaded = load_stores(path);
        if (!loaded) {
            error = loaded.error().message;
        } else if (!newest ||
                   loaded->installed.meta.build_time_ns > newest->installed.meta.build_time_ns) {
            newest = std::move(*loaded);
            chosen = path;
        }
    }
    if (!newest) {
        err << "egraph: complete: " << error << '\n';
        return Exit::failure;
    }
    // Only for the words that need it: it is the biggest of the three.
    std::optional<RepositoryIndex> index;
    if (needs_index(command.what, command.word)) {
        if (auto loaded = load_repository(repository_index_path(chosen))) {
            index = std::move(*loaded);
        }
    }
    std::optional<std::reference_wrapper<const RepositoryIndex>> view;
    if (index) {
        view = std::cref(*index);
    }
    write_lines(
        out, complete_word(newest->installed, newest->evaluated, view, command.what, command.word));
    return Exit::ok;
}

// The time zone times are read and shown in; UTC without one.
const std::chrono::time_zone& local_zone() {
    try {
        return *std::chrono::current_zone();
    } catch (const std::runtime_error&) {
        return *std::chrono::locate_zone("UTC");
    }
}

Exit execute(const LogCommand& command, Session&, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    std::vector<log::Event> events;
    if (log::targets(invocation.log, log::journal_running()).journal) {
        const auto printed = output_of(log::journal_command());
        if (!printed) {
            err << "egraph: log: journalctl failed: " << printed.error() << '\n';
            return Exit::failure;
        }
        events = log::journal_events(*printed);
    } else {
        const auto file =
            invocation.log_file.value_or(log::default_file(invocation.eprefix.value_or("")));
        std::ostringstream text;
        if (std::ifstream in{file}; in) {
            text << in.rdbuf();
        }
        events = log::file_events(text.str());
    }
    const auto runs = log::summarize(events);
    const auto* zone = &local_zone();
    const bool human = output(invocation).human;
    if (!command.run) {
        for (const auto& run : runs) {
            out << (human ? log::summary_line(run, *zone) : log::summary_fields(run)) << '\n';
        }
        return Exit::ok;
    }
    const auto found = log::find_run(runs, *command.run);
    if (!found) {
        err << "egraph: log: " << found.error() << '\n';
        return Exit::failure;
    }
    for (const auto& event : events) {
        if (event.run == *found) {
            out << (human ? log::event_line(event, *zone) : log::file_line(event)) << '\n';
        }
    }
    return Exit::ok;
}

Exit execute(const Diff& command, Session& session, const Invocation& invocation, std::ostream& out,
             std::ostream& err) {
    using namespace std::chrono;
    const auto& zone = local_zone();
    DiffBase base;
    if (command.base) {
        auto parsed = parse_diff_base(*command.base, floor<seconds>(system_clock::now()), zone);
        if (!parsed) {
            err << "egraph: diff: " << parsed.error() << '\n';
            return Exit::usage;
        }
        base = std::move(*parsed);
    }
    const auto current = session.installed();
    if (!current) {
        return fail(err, current.error());
    }
    const auto directory = history_directory(invocation.root, invocation.eprefix.value_or(""));
    std::vector<Seconds> generations;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        if (const auto ended = generation_time(entry.path().filename().string())) {
            generations.push_back(*ended);
        }
    }
    const auto ended =
        base.generation ? generation_time(*base.generation) : generation_at(generations, base.at);
    if (!ended && !base.at) {
        err << std::format("egraph: diff: no generations in {} yet; a refresh of the system store "
                           "keeps one when it changes the installed packages or the root sets\n",
                           directory.string());
        return Exit::failure;
    }
    const auto shown = [&zone](Seconds time) {
        return std::format("{:%Y-%m-%d %H:%M:%S}", zoned_time{&zone, time});
    };
    // Without a generation, the system is as it was then.
    std::optional<Store> old;
    Seconds since = base.at.value_or(Seconds{});
    if (ended) {
        const auto path = directory / generation_name(*ended);
        auto loaded = load(path);
        if (!loaded) {
            const auto& mismatch = loaded.error().mismatch;
            err << "egraph: diff: "
                << (mismatch ? std::format("{} is of store format {}, from another egraph version, "
                                           "which this one cannot read",
                                           path.string(), mismatch->found)
                             : loaded.error().message)
                << '\n';
            return Exit::failure;
        }
        old = std::move(*loaded);
        if (!base.at) {
            // The last time a refresh found the system so.
            since = floor<seconds>(sys_time<nanoseconds>{nanoseconds{old->meta.build_time_ns}});
        } else if (*ended == std::ranges::min(generations)) {
            // Before the oldest generation's newest merge, the system was as no generation holds.
            const auto newest = std::ranges::max(old->packages, {}, &Package::merged).merged;
            if (const Seconds start{seconds{newest}};
                old->packages.size() != 0 && *base.at < start) {
                err << std::format(
                    "egraph: diff: the history starts at {}, so this is since then\n",
                    shown(start));
                since = start;
            }
        }
    }
    const auto changes = old ? differences(*old, *current) : std::vector<Difference>{};
    if (command.json) {
        const auto name = ended ? std::optional{generation_name(*ended)} : std::nullopt;
        write_differences_json(out, changes, since, name);
        return Exit::ok;
    }
    const auto lines = difference_lines(changes);
    if (const auto style = output(invocation); style.human) {
        human_diff(out, lines, shown(since), style.theme);
    } else {
        write_lines(out, lines);
    }
    return Exit::ok;
}

// A status file and whether it is current against the stores beside it.
struct FoundStatus {
    std::filesystem::path path;
    Status status;
    bool current = false;
};

FoundStatus found_status(const std::filesystem::path& path, Status status) {
    const bool current = stores_unchanged(status.stores);
    return {.path = path, .status = std::move(status), .current = current};
}

// The status file beside --store; without, the system store's or the user's, whichever is
// current, else the newer. The error names where none was found.
std::expected<FoundStatus, std::string> find_status(const Invocation& invocation) {
    std::vector<std::filesystem::path> stores;
    if (invocation.store) {
        stores.push_back(*invocation.store);
    } else {
        stores.push_back(system_store_path(invocation));
        if (const auto user = user_store_path(invocation); user && *user != stores.front()) {
            stores.push_back(*user);
        }
    }
    std::optional<FoundStatus> found;
    std::string error;
    for (const auto& installed : stores) {
        auto status = read_status(status_path(installed));
        if (!status) {
            if (std::filesystem::exists(status_path(installed))) {
                error = status.error();
            }
            continue;
        }
        auto candidate = found_status(status_path(installed), std::move(*status));
        if (!found || (candidate.current && !found->current) ||
            (candidate.current == found->current &&
             candidate.status.written > found->status.written)) {
            found = std::move(candidate);
        }
    }
    if (found) {
        return std::move(*found);
    }
    if (!error.empty()) {
        return std::unexpected(std::move(error));
    }
    return std::unexpected(std::format("no status file at {}; egraphd writes it as it refreshes "
                                       "the stores, or egraph status --update now",
                                       status_path(stores.front()).string()));
}

Exit execute(const StatusCommand& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    std::expected<FoundStatus, std::string> found = std::unexpected(std::string{});
    if (command.update) {
        const auto now = session_build_times(session);
        if (!now) {
            return fail(err, now.error());
        }
        const auto path = status_path(session.used());
        if (const auto error = write_status(session, invocation, *now, path)) {
            err << "egraph: status: " << *error << '\n';
            return Exit::failure;
        }
        found = read_status(path).transform(
            [&path](Status status) { return found_status(path, std::move(status)); });
    } else {
        found = find_status(invocation);
    }
    if (!found) {
        err << "egraph: status: " << found.error() << '\n';
        return Exit::failure;
    }
    if (command.json) {
        out << status_json(found->status, found->current);
    } else if (style(invocation).human) {
        write_lines(out, status_summary(found->status, found->current,
                                        std::chrono::floor<std::chrono::seconds>(
                                            std::chrono::system_clock::now())));
    } else {
        write_lines(out, status_lines(found->status, found->current));
    }
    return Exit::ok;
}

Exit execute(const HistoryCommand& command, Session& session, const Invocation& invocation,
             std::ostream& out, std::ostream& err) {
    using namespace std::chrono;
    const auto& zone = local_zone();
    const auto query =
        parse_history_query(command.arguments, floor<seconds>(system_clock::now()), zone);
    if (!query) {
        err << "egraph: history: " << query.error() << '\n';
        return Exit::usage;
    }
    // The refresh logs what changed since the last one.
    const auto loaded = session.installed();
    if (!loaded) {
        return fail(err, loaded.error());
    }
    const Store& current = *loaded;
    const auto directory = history_directory(invocation.root, invocation.eprefix.value_or(""));
    std::ostringstream text;
    if (std::ifstream in{directory / "history.log"}; in) {
        text << in.rdbuf();
    }
    const auto events = selected_events(parse_history(text.str()), *query);
    if (!events) {
        err << "egraph: history: " << events.error() << '\n';
        return Exit::usage;
    }
    const auto style = output(invocation);
    if (!style.human) {
        for (const auto& event : *events) {
            out << event_line(event) << '\n';
        }
        return Exit::ok;
    }
    human_history(out, event_records(*events, zone), style.theme);
    if (query->packages.empty()) {
        return Exit::ok;
    }
    std::vector<Seconds> generations;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        if (const auto ended = generation_time(entry.path().filename().string())) {
            generations.push_back(*ended);
        }
    }
    std::ranges::sort(generations);
    const auto find = [](const Store& store, std::string_view cpv) -> std::optional<std::uint32_t> {
        for (std::uint32_t id = 0; id < store.packages.size(); ++id) {
            if (store.string(store.packages.at(id).cpv) == cpv) {
                return id;
            }
        }
        return std::nullopt;
    };
    const auto& paint = style.theme.paint;
    bool paths = false;
    for (const auto& arrival : arrivals(*events)) {
        // Why it arrived: the first generation holding it, as the system was just after.
        std::optional<Store> then;
        for (const auto ended : generations) {
            if (ended <= arrival.time) {
                continue;
            }
            if (auto generation = load(directory / generation_name(ended));
                generation && find(*generation, arrival.cpv)) {
                then = std::move(*generation);
                break;
            }
        }
        const Store& store = then ? *then : current;
        const auto merged = std::format("{:%Y-%m-%d %H:%M:%S}", zoned_time{&zone, arrival.time});
        const auto id = find(store, arrival.cpv);
        out << '\n';
        if (!id) {
            out << paint(std::format("{} merged {}; no generation holds it, nor the system now",
                                     arrival.cpv, merged),
                         Tone::note)
                << '\n';
            continue;
        }
        out << paint(then
                         ? std::format("{} merged {}; then:", arrival.cpv, merged)
                         : std::format("{} merged {}; no generation holds it, so now:", arrival.cpv,
                                       merged),
                     Tone::heading)
            << '\n';
        const auto path = why(keep(store, {}), *id);
        if (!path) {
            out << paint("nothing: depclean would have removed it", Tone::note) << '\n';
            continue;
        }
        human_path(out, path_lines(store, *path), style.theme);
        paths = true;
    }
    if (paths) {
        human_legend(out, style.theme);
    }
    return Exit::ok;
}

Exit execute(const Export& command, Session& session, const Invocation&, std::ostream& out,
             std::ostream& err) {
    if (command.repository) {
        if (command.format != ExportFormat::json || !command.packages.empty() ||
            command.evaluated) {
            err << "egraph: export --repository writes the whole index, as JSON\n";
            return Exit::usage;
        }
        const auto index = session.repository();
        if (!index) {
            return fail(err, index.error());
        }
        write_repository_json(out, *index);
        return Exit::ok;
    }
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
    app.add_option("--replace-slots", invocation.replace_slots,
                   "Packages whose old slots a plan replaces (default: etc/egraph/replace-slots "
                   "under the configuration root)")
        ->type_name("FILE")
        ->envname("EGRAPH_REPLACE_SLOTS");
    app.add_option("--builder", invocation.builder,
                   "egraph-build command that refreshes the store (default: the one next to "
                   "egraph, else egraph-build in PATH)")
        ->type_name("COMMAND")
        ->envname("EGRAPH_BUILD");
    app.add_option("--emerge", invocation.emerge,
                   "emerge command that --verify runs (default: emerge in PATH)")
        ->type_name("COMMAND")
        ->envname("EGRAPH_EMERGE");
    app.add_option("--dispatch-conf", invocation.dispatch_conf,
                   "Command an action offers for configuration updates (default: dispatch-conf "
                   "in PATH)")
        ->type_name("COMMAND")
        ->envname("EGRAPH_DISPATCH_CONF");
    app.add_option("--emaint", invocation.emaint,
                   "emaint command that sync runs (default: emaint in PATH)")
        ->type_name("COMMAND")
        ->envname("EGRAPH_EMAINT");
    app.add_flag("--no-refresh", invocation.no_refresh,
                 "Answer from a stale store instead of rebuilding it");
    app.add_option("--layout", invocation.layout,
                   "Results for people, or as tab-separated lines for scripts (default auto: "
                   "for people on a terminal)")
        ->transform(one_of<Layout>(
            {{"auto", Layout::automatic}, {"human", Layout::human}, {"lines", Layout::lines}}))
        ->envname("EGRAPH_LAYOUT");
    app.add_option("--log", invocation.log,
                   "Where runs that change the system are logged (default auto: the systemd "
                   "journal when systemd runs, else the file)")
        ->transform(one_of<log::Sink>({{"auto", log::Sink::automatic},
                                       {"journal", log::Sink::journal},
                                       {"file", log::Sink::file},
                                       {"both", log::Sink::both},
                                       {"none", log::Sink::none}}))
        ->envname("EGRAPH_LOG");
    app.add_option("--log-file", invocation.log_file,
                   "The log file (default: ${EPREFIX}/var/log/egraph.log)")
        ->type_name("FILE")
        ->envname("EGRAPH_LOG_FILE");
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
    CLI::App* search_cmd = add_command<Search>(
        app, invocation,
        "Packages in the repositories or installed whose names match, as emerge --search");
    add_field(search_cmd, invocation, "keys", &Search::keys,
              "Search keys: text, a regular expression after %, a category after @ or with a /")
        ->required();
    const auto options = [&invocation]() -> SearchOptions& {
        return std::get<Search>(invocation.command).options;
    };
    search_cmd->add_flag_callback(
        "-S,--searchdesc", [options] { options().description = true; },
        "Match descriptions too, as emerge's option");
    search_cmd
        ->add_option_function<bool>(
            "--fuzzy-search", [options](const bool& value) { options().fuzzy = value; },
            "Also names alike, as emerge's option (default y)")
        ->transform(yes_no);
    search_cmd
        ->add_option_function<bool>(
            "--regex-search-auto", [options](const bool& value) { options().regex_auto = value; },
            "Take a key that looks like a regular expression for one, as emerge's option "
            "(default y)")
        ->transform(yes_no);
    search_cmd
        ->add_option_function<std::uint32_t>(
            "--search-similarity",
            [options](const std::uint32_t& value) { options().similarity = value; },
            "How alike a fuzzy match must be, in percent, as emerge's option (default 80)")
        ->type_name("N")
        ->check(CLI::Range(0, 100));
    add_field(add_command<Versions>(app, invocation,
                                    "Every version in the repositories of packages, with its "
                                    "slot, repository and why it is masked"),
              invocation, "packages", &Versions::packages,
              "Atoms, or names without a category; every version without any")
        ->type_name("ATOM");
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
    CLI::App* exec_cmd = add_dynamic_deps(add_command<Exec>(
        app, invocation,
        "Show the plan for a request, then merge it one package at a time through egraph-build "
        "--worker once confirmed"));
    add_plan_options<Exec>(exec_cmd, invocation);
    exec_cmd->add_flag_callback(
        "-1,--oneshot", [&invocation] { std::get<Exec>(invocation.command).oneshot = true; },
        "Add the targets to no set, as emerge --oneshot");
    add_yes<Exec>(exec_cmd, invocation,
                  "Merge without asking, as scripts must where there is no terminal to ask on");
    exec_cmd
        ->add_option_function<std::uint32_t>(
            "-j,--jobs",
            [&invocation](const std::uint32_t& jobs) {
                std::get<Exec>(invocation.command).jobs = jobs;
            },
            "Build up to N packages at once, as emerge --jobs; EMERGE_DEFAULT_OPTS' count "
            "otherwise")
        ->type_name("N")
        ->check(CLI::PositiveNumber);
    exec_cmd
        ->add_option_function<std::string>(
            "--trace",
            [&invocation](const std::string& path) {
                std::get<Exec>(invocation.command).trace = path;
            },
            "Write to FILE when each build, merge and uninstall starts and ends, seconds from the "
            "start, a line each")
        ->type_name("FILE");
    exec_cmd
        ->add_option_function<bool>(
            "--keep-going",
            [&invocation](const bool& value) {
                std::get<Exec>(invocation.command).keep_going = value;
            },
            "Go on after a failure without what needs the failed package, as emerge's option "
            "(y without a value); EMERGE_DEFAULT_OPTS' otherwise")
        ->expected(0, 1)
        ->default_str("y")
        ->transform(yes_no);
    exec_cmd
        ->add_option_function<std::string>(
            "--merge-wait-scope",
            [&invocation](const std::string& scope) {
                std::get<Exec>(invocation.command).merge_wait_scope = scope;
            },
            "The packages that merge alone once no build runs, whatever FEATURES=merge-wait says: "
            "deep (@system and what it needs at run time), system, toolchain or none; "
            "EMERGE_DEFAULT_OPTS' otherwise, else deep")
        ->type_name("SCOPE")
        ->check(CLI::IsMember({"deep", "system", "toolchain", "none"}));
    // Targets stay required while parsing, so that --keep-going takes none for its value, unless
    // --resume, which takes the last run's.
    CLI::Option* exec_targets = exec_cmd->get_option("targets");
    exec_cmd->preparse_callback([&invocation, exec_targets](std::size_t) {
        invocation.command.emplace<Exec>();
        exec_targets->required();
    });
    exec_cmd
        ->add_flag_callback(
            "--resume",
            [&invocation, exec_targets] {
                std::get<Exec>(invocation.command).resume = true;
                exec_targets->required(false);
            },
            "Carry out the last run's targets and options again, without what it merged; --jobs, "
            "--keep-going and --merge-wait-scope given now replace its own")
        ->trigger_on_parse();
    exec_cmd->callback([&invocation] {
        const auto& exec = std::get<Exec>(invocation.command);
        if (exec.resume && (!exec.targets.empty() || exec.update || exec.deep || exec.noreplace ||
                            exec.oneshot || exec.rebuilds != UseRebuilds::none)) {
            throw CLI::ValidationError("--resume", "takes the last run's targets and plan options");
        }
    });
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
        ->type_name("ATOM")
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
    CLI::App* sync_cmd = add_dynamic_deps(add_command<Sync>(
        app, invocation,
        "Have emaint sync the repositories, then show the updates and the notices"));
    add_updates_options<Sync>(sync_cmd, invocation);
    add_field(sync_cmd, invocation, "repositories", &Sync::repositories,
              "Repositories to sync, by name or alias (default: those set to auto-sync)")
        ->type_name("REPOSITORY");

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
    export_cmd->add_flag_callback(
        "--repository", [&invocation] { std::get<Export>(invocation.command).repository = true; },
        "Export the repository index instead, whole and as JSON: every version in the "
        "repositories, and what decides their visibility");
    add_field(export_cmd, invocation, "--depth", &Export::depth,
              "Dependency edges to follow out from the packages (default 1)");
    add_field(export_cmd, invocation, "--direction", &Export::direction,
              "Follow reverse dependencies, forward ones, or both (default reverse)")
        ->transform(one_of<Direction>({{"reverse", Direction::reverse},
                                       {"forward", Direction::forward},
                                       {"both", Direction::both}}));

    add_command<NoticesCommand>(
        app, invocation,
        "What needs attention once emerge has run: configuration updates waiting, unread news");
    add_command<Stats>(app, invocation, "Store and graph statistics");
    add_field(add_command<LogCommand>(app, invocation,
                                      "The runs logged where --log writes, a line each, or the "
                                      "events of one"),
              invocation, "run", &LogCommand::run, "A run's id, or the start of it")
        ->type_name("RUN");
    CLI::App* diff_cmd = add_command<Diff>(
        app, invocation,
        "The installed packages and root sets now against the system as the history kept it");
    add_field(diff_cmd, invocation, "when", &Diff::base,
              "An age (12h, 3d, 2w), a date (2026-09-30) or a generation's file name; the newest "
              "generation when omitted")
        ->type_name("WHEN");
    diff_cmd->add_flag_callback(
        "--json", [&invocation] { std::get<Diff>(invocation.command).json = true; },
        "Write the changes as JSON");
    add_field(add_command<HistoryCommand>(
                  app, invocation,
                  "The system store's history log: merges, upgrades, downgrades, rebuilds and "
                  "uninstalls; for packages, also what pulled each in"),
              invocation, "arguments", &HistoryCommand::arguments,
              "An age (12h, 3d, 2w) or a date (2026-09-30) to start from, and installed cpvs "
              "or atoms")
        ->type_name("WHEN|PACKAGE");
    CLI::App* status_cmd = add_command<StatusCommand>(
        app, invocation,
        "The updates egraph watch last planned, counted, and each repository's last sync, "
        "without planning anything");
    status_cmd->add_flag_callback(
        "--json", [&invocation] { std::get<StatusCommand>(invocation.command).json = true; },
        "Write the status file, and whether it is current, as JSON");
    status_cmd->add_flag_callback(
        "--update", [&invocation] { std::get<StatusCommand>(invocation.command).update = true; },
        "Plan the updates and write the status file first");
    add_command<Tui>(app, invocation, "Browse the graph in a terminal interface");
    add_command<Shell>(app, invocation,
                       "Answer commands read one per line from standard input, loading the stores "
                       "once");
    add_command<Rebuild>(app, invocation, "Rebuild the store from scratch");
    add_command<Refresh>(app, invocation,
                         "Bring the stores and the repository index up to date if their inputs "
                         "changed, printing nothing");
    add_command<Watch>(app, invocation,
                       "Keep the stores and the repository index fresh as their inputs change, "
                       "until stopped");
    add_command<Check>(app, invocation, "Diff the store against a fresh build");
    CLI::App* complete_cmd = add_command<Complete>(
        app, invocation, "The words a shell completes a package argument to, from the stores");
    // Hidden: the completion scripts' own.
    complete_cmd->group("");
    complete_cmd->add_flag_callback(
        "--installed",
        [&invocation] { std::get<Complete>(invocation.command).what = Completing::installed; },
        "Installed packages, not the repositories'");
    complete_cmd->add_flag_callback(
        "--repositories",
        [&invocation] { std::get<Complete>(invocation.command).what = Completing::repositories; },
        "Repository names");
    add_field(complete_cmd, invocation, "word", &Complete::word, "The word typed so far");
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

std::filesystem::path config_root(const Invocation& invocation) {
    return invocation.config_root.value_or(invocation.eprefix.value_or(std::filesystem::path{"/"}));
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

// argv under the invocation's roots, for a portage tool that takes them from the environment only.
std::vector<std::string> with_environment_roots(const Invocation& invocation,
                                                std::vector<std::string> argv) {
    std::vector<std::string> roots;
    if (invocation.root != "/") {
        roots.push_back("ROOT=" + invocation.root.string());
    }
    if (invocation.config_root) {
        roots.push_back("PORTAGE_CONFIGROOT=" + invocation.config_root->string());
    }
    if (invocation.eprefix) {
        roots.push_back("PORTAGE_OVERRIDE_EPREFIX=" + invocation.eprefix->string());
    }
    if (!roots.empty()) {
        roots.insert(roots.begin(), "env");
        argv.insert(argv.begin(), roots.begin(), roots.end());
    }
    return argv;
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

std::vector<std::string> worker_command(const Invocation& invocation) {
    std::vector<std::string> argv{builder_program(invocation), "--worker"};
    add_roots(argv, invocation);
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

std::vector<std::string> egraph_options(const Invocation& invocation) {
    std::vector<std::string> options{"--root", invocation.root.string()};
    const auto pass = [&options](std::string_view name, const auto& value) {
        if (value) {
            options.emplace_back(name);
            if constexpr (std::is_same_v<std::decay_t<decltype(*value)>, std::filesystem::path>) {
                options.push_back(value->string());
            } else {
                options.emplace_back(*value);
            }
        }
    };
    pass("--config-root", invocation.config_root);
    pass("--eprefix", invocation.eprefix);
    pass("--store", invocation.store);
    pass("--replace-slots", invocation.replace_slots);
    pass("--builder", invocation.builder);
    pass("--emerge", invocation.emerge);
    pass("--dispatch-conf", invocation.dispatch_conf);
    pass("--emaint", invocation.emaint);
    if (invocation.no_refresh) {
        options.emplace_back("--no-refresh");
    }
    constexpr std::array<std::string_view, 5> sinks{"auto", "journal", "file", "both", "none"};
    if (invocation.log != log::Sink::automatic) {
        options.insert(options.end(),
                       {"--log", std::string{sinks.at(std::to_underlying(invocation.log))}});
    }
    pass("--log-file", invocation.log_file);
    if (invocation.glyphs) {
        constexpr std::array<std::string_view, 3> glyphs{"nerd", "unicode", "ascii"};
        options.insert(
            options.end(),
            {"--glyphs", std::string{glyphs.at(std::to_underlying(*invocation.glyphs))}});
    }
    return options;
}

std::vector<std::string> emerge_options_command(const Invocation& invocation,
                                                const std::filesystem::path& output) {
    std::vector<std::string> argv{builder_program(invocation), "--emerge-options", "--output",
                                  output.string()};
    add_roots(argv, invocation);
    return argv;
}

std::vector<std::string> notices_command(const Invocation& invocation,
                                         const std::filesystem::path& output) {
    std::vector<std::string> argv{builder_program(invocation), "--notices", "--output",
                                  output.string()};
    add_roots(argv, invocation);
    return argv;
}

std::vector<std::string> copy_portage_command(const Invocation& invocation,
                                              const std::filesystem::path& directory) {
    std::vector<std::string> argv{builder_program(invocation), "--copy-portage", "--output",
                                  directory.string()};
    add_roots(argv, invocation);
    return argv;
}

std::vector<std::string> exec_arguments(const Exec& command) {
    std::vector<std::string> arguments;
    const auto flag = [&arguments](bool set, const char* name) {
        if (set) {
            arguments.emplace_back(name);
        }
    };
    flag(command.update, "--update");
    flag(command.deep, "--deep");
    flag(command.noreplace, "--noreplace");
    flag(command.rebuilds == UseRebuilds::all, "--newuse");
    flag(command.rebuilds == UseRebuilds::changed, "--changed-use");
    flag(command.oneshot, "--oneshot");
    if (command.jobs) {
        arguments.insert(arguments.end(), {"--jobs", std::to_string(*command.jobs)});
    }
    if (command.keep_going) {
        arguments.emplace_back(*command.keep_going ? "--keep-going=y" : "--keep-going=n");
    }
    if (command.merge_wait_scope) {
        arguments.insert(arguments.end(), {"--merge-wait-scope", *command.merge_wait_scope});
    }
    arguments.emplace_back("--");
    arguments.insert(arguments.end(), command.targets.begin(), command.targets.end());
    return arguments;
}

std::expected<Exec, std::string> resumed_exec(const Exec& given,
                                              std::span<const std::string> arguments) {
    CLI::App app;
    Invocation invocation;
    configure(app, invocation);
    // CLI11 takes the arguments last first.
    std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());
    reversed.emplace_back(Exec::name);
    try {
        app.parse(reversed);
    } catch (const CLI::ParseError& error) {
        return std::unexpected(
            std::format("the last run's arguments do not parse: {}", error.what()));
    }
    auto* parsed = std::get_if<Exec>(&invocation.command);
    if (parsed == nullptr || parsed->resume) {
        return std::unexpected("the last run's arguments are not a run's");
    }
    Exec resumed = std::move(*parsed);
    resumed.jobs = given.jobs ? given.jobs : resumed.jobs;
    resumed.keep_going = given.keep_going ? given.keep_going : resumed.keep_going;
    resumed.merge_wait_scope =
        given.merge_wait_scope ? given.merge_wait_scope : resumed.merge_wait_scope;
    resumed.yes = given.yes;
    resumed.trace = given.trace;
    return resumed;
}

std::vector<std::string> dispatch_conf_command(const Invocation& invocation) {
    return with_environment_roots(invocation, {invocation.dispatch_conf.value_or("dispatch-conf")});
}

std::vector<std::string> sync_command(const Invocation& invocation,
                                      const std::vector<std::string>& repositories) {
    std::vector<std::string> command{invocation.emaint.value_or("emaint"), "sync"};
    if (repositories.empty()) {
        command.emplace_back("--auto");
    } else {
        // emaint's --repo takes one value and splits it, as emerge --sync hands it the names.
        command.emplace_back("--repo");
        command.push_back(repositories | std::views::join_with(' ') |
                          std::ranges::to<std::string>());
    }
    return with_environment_roots(invocation, std::move(command));
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

std::vector<std::string> kernel_sources_command(const Invocation& invocation,
                                                const std::filesystem::path& output,
                                                const std::vector<std::string>& cpvs) {
    std::vector<std::string> argv{builder_program(invocation), "--kernel-sources", "--output",
                                  output.string()};
    add_roots(argv, invocation);
    argv.insert(argv.end(), cpvs.begin(), cpvs.end());
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
    const auto usage = [&](std::string_view message) {
        err << "egraph: " << where << ": " << message << '\n';
        return LineResult{.quit = false, .exit = Exit::usage};
    };
    // CLI11 only says a subcommand is required.
    if (const auto word = line.substr(0, line.find_first_of(" \t"));
        !word.starts_with('-') && app.get_subcommand_no_throw(std::string{word}) == nullptr) {
        return usage(std::format("{}: no such command (help lists them)", word));
    }
    try {
        app.parse(std::string{line}, false);
    } catch (const CLI::ParseError& e) {
        // --help arrives here too, with exit code 0.
        return {.quit = false, .exit = app.exit(e, out, err) == 0 ? Exit::ok : Exit::usage};
    }
    if (const auto option = changed_stores(invocation, command)) {
        return usage(std::format("{} chooses the stores, which the {} keeps; start another "
                                 "egraph for others",
                                 *option, where));
    }
    if (std::holds_alternative<std::monostate>(command.command) ||
        std::holds_alternative<Shell>(command.command) ||
        std::holds_alternative<Tui>(command.command)) {
        return usage(std::format("already in the {}", where));
    }
    if (std::holds_alternative<Watch>(command.command)) {
        return usage("watch runs on its own until stopped, as the egraphd service runs it");
    }
    // emerge would write over the interface's screen.
    if (context == Context::interface && (std::holds_alternative<Update>(command.command) ||
                                          std::holds_alternative<Install>(command.command) ||
                                          std::holds_alternative<Exec>(command.command) ||
                                          std::holds_alternative<Remove>(command.command) ||
                                          std::holds_alternative<Select>(command.command) ||
                                          std::holds_alternative<Deselect>(command.command) ||
                                          std::holds_alternative<Sync>(command.command))) {
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

std::vector<std::string> multicall_arguments(CLI::App& app, std::string_view program,
                                             std::vector<std::string> arguments) {
    constexpr std::string_view prefix = "egraph-";
    const auto name = std::filesystem::path{program}.filename().string();
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return arguments;
    }
    auto command = name.substr(prefix.size());
    auto* sub = app.get_subcommand_no_throw(command);
    if (sub == nullptr) {
        return arguments;
    }
    // Its arguments come all together, the global options among them.
    sub->fallthrough();
    arguments.insert(arguments.begin(), std::move(command));
    return arguments;
}

Exit run(const Invocation& invocation, std::ostream& out, std::ostream& err) {
    Session session{invocation, err};
    auto asking = invocation;
    asking.ask = invocation.terminal && invocation.input_terminal;
    return dispatch(session, asking, out, err);
}

} // namespace egraph
