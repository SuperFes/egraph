#include "session.hpp"

#include "freshness.hpp"
#include "history.hpp"
#include "os.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <ostream>
#include <thread>
#include <utility>

namespace egraph {

namespace {

// What loads from path and is current, or nullopt. Loaded is a Store or Stores.
template <class Loaded, class Load>
std::optional<Loaded> fresh(const std::filesystem::path& path, const Load& load_path) {
    auto loaded = load_path(path);
    if (loaded && !staleness(*loaded)) {
        return std::move(*loaded);
    }
    return std::nullopt;
}

// What load_path reads from the store, refreshed through egraph-build first if its inputs
// changed; used is the installed store it came from. Without --store, a current system store is
// read even when only root can refresh it; otherwise the user's own store is kept current
// instead.
template <class Loaded, class Load>
std::expected<Loaded, std::string> open_current(const Invocation& invocation, std::ostream& err,
                                                std::filesystem::path& used, const Load& load_path,
                                                std::string_view mode = "--incremental") {
    const auto path = store_path(invocation);
    used = path;
    if (!invocation.store) {
        const auto system = system_store_path(invocation);
        if (system != path) {
            if (auto loaded = fresh<Loaded>(system, load_path)) {
                used = system;
                return std::move(*loaded);
            }
        }
    }
    auto loaded = load_path(path);
    if (loaded) {
        const auto reason = staleness(*loaded);
        if (!reason) {
            return std::move(*loaded);
        }
        if (invocation.no_refresh) {
            err << "egraph: warning: answering from a stale store (" << *reason << ")\n";
            return std::move(*loaded);
        }
    } else if (invocation.no_refresh) {
        return std::unexpected(stored_store_error(loaded.error()));
    }
    if (loaded) {
        // Else a refresh right after an emerge writes a store the next query refreshes again.
        const std::chrono::nanoseconds now = std::chrono::system_clock::now().time_since_epoch();
        std::this_thread::sleep_for(settle_wait(
            *loaded, static_cast<std::uint64_t>(std::max<std::int64_t>(now.count(), 0))));
    }
    if (auto error = refresh_store(invocation, mode, path, err)) {
        return std::unexpected(std::move(*error));
    }
    return load_path(path).transform_error([&invocation](const StoreError& error) {
        return built_store_error(builder_program(invocation), error);
    });
}

} // namespace

std::optional<std::string> run_builder(const Invocation& invocation, std::string_view mode,
                                       const std::filesystem::path& path,
                                       const std::optional<std::filesystem::path>& log,
                                       std::span<const std::string> cps) {
    return builder_error(invocation, os::run(builder_command(invocation, mode, path, cps), log));
}

namespace {

// Records what changed from before, the store a refresh replaced at path, in directory: the
// events in its log, and with days, before itself as a generation, the generations thinned.
void record_history(const std::vector<std::byte>& before, const std::filesystem::path& path,
                    const std::filesystem::path& directory, int days, std::ostream& warnings) {
    namespace fs = std::filesystem;
    using namespace std::chrono;
    // A store from another format version is not history this egraph can read.
    const auto old = decode(before);
    const auto current = load(path);
    if (!old || !current || !history_changed(*old, *current)) {
        return;
    }
    const auto now = floor<seconds>(system_clock::now());
    std::string lines;
    for (const auto& event : history_events(*old, *current, now)) {
        lines += event_line(event) + '\n';
    }
    const auto log = directory / "history.log";
    if (const auto logged = lines.empty() ? std::expected<void, std::error_code>{}
                                          : os::append_replacing(log, lines);
        !logged) {
        warnings << std::format("egraph: warning: cannot log to {}: {}\n", log.string(),
                                logged.error().message());
    }
    if (days == 0) {
        return;
    }
    // Named by when the system left the state it holds: its own build time is only the last
    // refresh that found that state unchanged.
    const auto ended =
        floor<seconds>(sys_time<nanoseconds>{nanoseconds{current->meta.build_time_ns}});
    std::string text(before.size(), '\0');
    std::ranges::transform(before, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    const auto generation = directory / generation_name(ended);
    if (const auto kept = os::replace_with_text(generation, text); !kept) {
        warnings << std::format("egraph: warning: cannot keep {}: {}\n", generation.string(),
                                kept.error().message());
        return;
    }
    std::vector<Seconds> generations;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator{directory, error}) {
        if (const auto time = generation_time(entry.path().filename().string())) {
            generations.push_back(*time);
        }
    }
    for (const auto gone : thinned(generations, now, days)) {
        fs::remove(directory / generation_name(gone), error);
    }
}

} // namespace

std::optional<std::string> refresh_store(const Invocation& invocation, std::string_view mode,
                                         const std::filesystem::path& path,
                                         std::ostream& warnings) {
    std::optional<std::vector<std::byte>> before;
    const auto directory = history_directory(invocation.root, invocation.eprefix.value_or(""));
    int days = 0;
    // The repository index's builds leave the installed store as it is.
    if (mode != "--repository" && !invocation.store && path == system_store_path(invocation) &&
        os::can_create(directory / "history.log")) {
        if (const auto settings = read_settings(settings_path(config_root(invocation)))) {
            days = settings->history_days;
        } else {
            warnings << "egraph: warning: " << settings.error() << ", so no generations are kept\n";
        }
        if (auto bytes = read_file(path)) {
            before = std::move(*bytes);
        }
    }
    if (auto error = run_builder(invocation, mode, path)) {
        return error;
    }
    if (before) {
        record_history(*before, path, directory, days, warnings);
    }
    return std::nullopt;
}

std::string built_store_error(std::string_view builder, const StoreError& error) {
    if (!error.mismatch) {
        return error.message;
    }
    const auto& mismatch = *error.mismatch;
    return std::format("egraph and {0} are from different versions: {0} wrote {1} as {2} of "
                       "format {3}, but this egraph reads format {4}; install both from the same "
                       "release",
                       builder, mismatch.path.string(), mismatch.kind, mismatch.found,
                       mismatch.expected);
}

std::string stored_store_error(const StoreError& error) {
    if (!error.mismatch) {
        return error.message;
    }
    const auto& mismatch = *error.mismatch;
    return std::format("{} is {} of format {}, from another egraph version, but this egraph reads "
                       "format {}; egraph rebuild writes it anew",
                       mismatch.path.string(), mismatch.kind, mismatch.found, mismatch.expected);
}

std::optional<std::string> builder_error(const Invocation& invocation,
                                         const std::expected<int, os::SpawnError>& status) {
    if (!status) {
        return std::format("cannot build the store: {} (install egraph-build, or name it with "
                           "--builder or EGRAPH_BUILD)",
                           status.error().message);
    }
    if (*status != 0) {
        return std::format("{} exited with status {}", builder_program(invocation), *status);
    }
    return std::nullopt;
}

std::optional<Stores> current_stores(const Invocation& invocation, std::filesystem::path& used) {
    const auto path = store_path(invocation);
    if (!invocation.store) {
        const auto system = system_store_path(invocation);
        if (auto loaded = system != path ? fresh<Stores>(system, load_stores) : std::nullopt) {
            used = system;
            return loaded;
        }
    }
    used = path;
    return fresh<Stores>(path, load_stores);
}

std::optional<RepositoryIndex> current_repository(const Invocation& invocation,
                                                  std::filesystem::path& used) {
    const auto load_index = [](const std::filesystem::path& path) {
        return load_repository(repository_index_path(path));
    };
    const auto path = store_path(invocation);
    if (!invocation.store) {
        const auto system = system_store_path(invocation);
        if (auto loaded =
                system != path ? fresh<RepositoryIndex>(system, load_index) : std::nullopt) {
            used = system;
            return loaded;
        }
    }
    used = path;
    return fresh<RepositoryIndex>(path, load_index);
}

std::expected<Stores, std::string> open_stores(const Invocation& invocation,
                                               std::ostream& warnings) {
    std::filesystem::path used;
    return open_current<Stores>(invocation, warnings, used, load_stores);
}

Session::Session(Invocation invocation, std::ostream& warnings)
    : invocation_{std::move(invocation)}, warnings_{warnings} {}

Loaded<Store> Session::installed() {
    if (installed_) {
        return std::cref(*installed_);
    }
    if (stores_) {
        return std::cref(stores_->installed);
    }
    auto loaded = open_current<Store>(invocation_, warnings_.get(), used_,
                                      [](const auto& path) { return load(path); });
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    installed_ = std::move(*loaded);
    return std::cref(*installed_);
}

Loaded<Stores> Session::stores() {
    return shared_stores().transform(
        [](const std::shared_ptr<const Stores>& stores) { return std::cref(*stores); });
}

std::expected<std::shared_ptr<const Stores>, std::string> Session::shared_stores() {
    if (!stores_) {
        auto loaded = open_current<Stores>(invocation_, warnings_.get(), used_, load_stores);
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        stores_ = std::make_shared<const Stores>(std::move(*loaded));
    }
    return stores_;
}

Loaded<RepositoryIndex> Session::repository() {
    if (!repository_) {
        // Beside whichever installed store is current, as the stores are.
        std::filesystem::path used;
        auto loaded = open_current<RepositoryIndex>(
            invocation_, warnings_.get(), used,
            [](const auto& path) { return load_repository(repository_index_path(path)); },
            "--repository");
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        repository_ = std::make_shared<const RepositoryIndex>(std::move(*loaded));
        repository_used_ = repository_index_path(used);
    }
    return std::cref(*repository_);
}

std::shared_ptr<const RepositoryIndex> Session::adopt_repository(RepositoryIndex index) {
    repository_ = std::make_shared<const RepositoryIndex>(std::move(index));
    return repository_;
}

void Session::reload() {
    repository_.reset();
    stores_.reset();
    installed_.reset();
    dynamic_.reset();
    graphs_ = {};
    depclean_ = {};
}

void Session::adopt(std::shared_ptr<const Stores> stores, std::filesystem::path used) {
    stores_ = std::move(stores);
    used_ = std::move(used);
    installed_.reset();
    dynamic_.reset();
    graphs_ = {};
    depclean_ = {};
}

std::optional<std::string> Session::evaluate(std::span<const std::string> cps) {
    const auto path = store_path(invocation_);
    if (auto error = run_builder(invocation_, "--evaluate", path, std::nullopt, cps)) {
        return error;
    }
    auto loaded = load_stores(path);
    if (!loaded) {
        return built_store_error(builder_program(invocation_), loaded.error());
    }
    adopt(std::make_shared<const Stores>(std::move(*loaded)), path);
    return std::nullopt;
}

Loaded<Store> Session::dependencies(bool dynamic) {
    if (!dynamic) {
        return installed();
    }
    if (!dynamic_) {
        const auto both = stores();
        if (!both) {
            return std::unexpected(both.error());
        }
        dynamic_ = with_dynamic_deps(both->get().installed, both->get().evaluated);
    }
    return std::cref(*dynamic_);
}

Loaded<Graph> Session::graph(bool dynamic) {
    auto& graph = graphs_.at(dynamic ? 1 : 0);
    if (!graph) {
        const auto store = dependencies(dynamic);
        if (!store) {
            return std::unexpected(store.error());
        }
        graph = build_graph(store->get());
    }
    return std::cref(*graph);
}

Loaded<Session::Depclean> Session::depclean(bool build_deps, bool dynamic) {
    auto& depclean = depclean_.at((build_deps ? 2U : 0U) + (dynamic ? 1U : 0U));
    if (!depclean) {
        const auto both = stores();
        if (!both) {
            return std::unexpected(both.error());
        }
        const auto store = dynamic ? dependencies(true) : Loaded<Store>{both->get().installed};
        if (!store) {
            return std::unexpected(store.error());
        }
        std::vector<Masking> masking;
        masking.reserve(both->get().evaluated.packages.size());
        for (const auto& pkg : both->get().evaluated.packages) {
            masking.push_back(
                {.masked = dynamic ? pkg.masked : pkg.vdb_masked, .visible = pkg.visible});
        }
        KeepOptions options{.build_deps = build_deps,
                            .masking = std::move(masking),
                            .removed = {},
                            .protect = {},
                            .dropped = {},
                            .without_selected = false};
        auto kept = keep(store->get(), options);
        depclean =
            Depclean{.store = *store, .options = std::move(options), .kept = std::move(kept)};
    }
    return std::cref(*depclean);
}

} // namespace egraph
