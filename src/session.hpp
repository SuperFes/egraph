#pragma once

// What the queries read, loaded once and kept for as long as egraph runs: a one-shot command
// answers from a session and exits; the interactive app keeps one.

#include "cli.hpp"
#include "depclean.hpp"
#include "evaluated.hpp"
#include "graph.hpp"
#include "os.hpp"
#include "store.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace egraph {

// A piece of a session, or why it could not be loaded. The reference lives as long as the
// session.
template <class T> using Loaded = std::expected<std::reference_wrapper<const T>, std::string>;

class Session {
  public:
    // Warnings (a store answered from stale, say) go to warnings, once each.
    Session(Invocation invocation, std::ostream& warnings EGRAPH_LIFETIMEBOUND);

    // The installed store alone, which needs no evaluated store. Once loaded it stays what this
    // returns, even after stores() loads both.
    [[nodiscard]] Loaded<Store> installed() EGRAPH_LIFETIMEBOUND;
    // The installed store and the evaluated store built with it, current together.
    [[nodiscard]] Loaded<Stores> stores() EGRAPH_LIFETIMEBOUND;
    // As stores(), shared with a caller that keeps them past the session's next adopt().
    [[nodiscard]] std::expected<std::shared_ptr<const Stores>, std::string> shared_stores();
    // Answers from stores, loaded from used, from now on: what was computed from the old ones,
    // and every reference this returned, goes.
    void adopt(std::shared_ptr<const Stores> stores, std::filesystem::path used);
    // What the dependency queries read: with dynamic, the installed store with the evaluated
    // store's dependency trees; without, installed().
    [[nodiscard]] Loaded<Store> dependencies(bool dynamic) EGRAPH_LIFETIMEBOUND;
    // The edges of dependencies(dynamic).
    [[nodiscard]] Loaded<Graph> graph(bool dynamic) EGRAPH_LIFETIMEBOUND;
    // What depclean keeps over the store it reads, which is stores()'s installed store without
    // dynamic, so that its packages line up with the evaluated store's masks.
    struct Depclean {
        std::reference_wrapper<const Store> store;
        // What kept was computed with.
        KeepOptions options;
        Kept kept;
    };
    [[nodiscard]] Loaded<Depclean> depclean(bool build_deps, bool dynamic) EGRAPH_LIFETIMEBOUND;

    // Has egraph-build evaluate cps into the store this invocation may write (the system store
    // as root, the user's otherwise) and answers from it from now on, as adopt(); the error when
    // the build or the load failed.
    [[nodiscard]] std::optional<std::string> evaluate(std::span<const std::string> cps);

    // Forgets the stores, so the next call loads them again, refreshing them if stale (after a
    // configuration change, say); every reference this returned goes.
    void reload();

    // Sends later warnings to warnings instead.
    void warn_to(std::ostream& warnings EGRAPH_KEPT_BY_THIS) { warnings_ = warnings; }

    // The installed store's path, once one is loaded: the system store or the user's.
    [[nodiscard]] const std::filesystem::path& used() const EGRAPH_LIFETIMEBOUND { return used_; }

  private:
    Invocation invocation_;
    std::reference_wrapper<std::ostream> warnings_;
    std::filesystem::path used_;
    std::optional<Store> installed_;
    std::shared_ptr<const Stores> stores_;
    std::optional<Store> dynamic_;
    std::array<std::optional<Graph>, 2> graphs_;
    // By build_deps, then dynamic.
    std::array<std::optional<Depclean>, 4> depclean_;
};

// Runs egraph-build, its output to log when given; the error when it could not run or did not
// succeed.
[[nodiscard]] std::optional<std::string>
run_builder(const Invocation& invocation, std::string_view mode, const std::filesystem::path& path,
            const std::optional<std::filesystem::path>& log = std::nullopt,
            std::span<const std::string> cps = {});

// Why a builder run failed, from how it ended; nothing when it succeeded.
[[nodiscard]] std::optional<std::string>
builder_error(const Invocation& invocation, const std::expected<int, os::SpawnError>& status);

// Stores a session could answer from without building: the system store while it is current,
// else the one at store_path(invocation) while it is; used says which, or where to build.
[[nodiscard]] std::optional<Stores> current_stores(const Invocation& invocation,
                                                   std::filesystem::path& used);

// Both stores as a session loads them, for a caller that keeps them itself.
[[nodiscard]] std::expected<Stores, std::string> open_stores(const Invocation& invocation,
                                                             std::ostream& warnings);

} // namespace egraph
