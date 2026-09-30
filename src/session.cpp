#include "session.hpp"

#include "freshness.hpp"
#include "os.hpp"

#include <format>
#include <ostream>
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
                                                std::filesystem::path& used,
                                                const Load& load_path) {
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
        return std::unexpected(loaded.error().message);
    }
    if (auto error = run_builder(invocation, "--incremental", path)) {
        return std::unexpected(std::move(*error));
    }
    return load_path(path).transform_error([](const StoreError& error) { return error.message; });
}

} // namespace

std::optional<std::string> run_builder(const Invocation& invocation, std::string_view mode,
                                       const std::filesystem::path& path,
                                       const std::optional<std::filesystem::path>& log) {
    return builder_error(invocation, os::run(builder_command(invocation, mode, path), log));
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

void Session::adopt(std::shared_ptr<const Stores> stores, std::filesystem::path used) {
    stores_ = std::move(stores);
    used_ = std::move(used);
    installed_.reset();
    dynamic_.reset();
    graphs_ = {};
    depclean_ = {};
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
        KeepOptions options{.build_deps = build_deps, .masking = std::move(masking), .removed = {}};
        auto kept = keep(store->get(), options);
        depclean =
            Depclean{.store = *store, .options = std::move(options), .kept = std::move(kept)};
    }
    return std::cref(*depclean);
}

} // namespace egraph
