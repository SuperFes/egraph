#pragma once

#include "evaluated.hpp"
#include "graph.hpp"
#include "store.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// "parent<TAB>kind<TAB>atom<TAB>child", with "<TAB>any-of" appended for an alternative inside a
// || group.
[[nodiscard]] std::string edge_line(const Store& store, const Edge& edge);

// deps and rdeps: one edge_line per edge, sorted, without duplicates.
[[nodiscard]] std::vector<std::string> edge_lines(const Store& store, std::span<const Edge> edges);

// What the ebuilds would add with flags toggled: edge_lines of the installed packages the possible
// dependencies of packages match (reverse: of every package, into packages, which is sorted), each
// with
// "<TAB>use=toggles" appended, the toggles space-separated. Sorted, without duplicates.
[[nodiscard]] std::vector<std::string>
possible_lines(const Evaluated& evaluated, std::span<const std::uint32_t> packages, bool reverse);

// The rebuilds for USE that updates lists beside replacements: none (as emerge -u), the ones
// --changed-use makes, or all --newuse makes.
enum class UseRebuilds : std::uint8_t { none, changed, all };

enum class UpdateKind : std::uint8_t { upgrade, downgrade, rebuild };

// What emerge -u @installed would do to an installed package.
struct PendingUpdate {
    // By the versions: the same version is a rebuild, for USE or because the installed one is
    // masked.
    UpdateKind kind = UpdateKind::upgrade;
    // Index into Evaluated::candidates.
    std::uint32_t target = 0;
    // A USE rebuild's flags in emerge's notation, space-separated, as far as rebuilds selects
    // them; empty for a replacement.
    std::string flags;
};

// The package's pending update, if emerge -u would replace it or, with rebuilds, rebuild it for
// USE.
[[nodiscard]] std::optional<PendingUpdate>
pending_update(const Evaluated& evaluated, std::uint32_t package, UseRebuilds rebuilds);

// An installed dependent's atom that rejects an update's target.
struct Holder {
    std::uint32_t parent = 0;
    // String id of the atom in the store the dependencies were read from.
    std::uint32_t atom = 0;
    auto operator<=>(const Holder&) const = default;
};

// A pending update weighed against the installed dependents' atoms, as emerge -uD weighs it.
struct WeighedUpdate {
    // What emerge would do: the pending update, or an update to the best visible version in the
    // slot every dependent accepts; nothing when none does.
    std::optional<PendingUpdate> update;
    // The pending update when dependents reject it, and the atoms that do, sorted.
    std::optional<PendingUpdate> held;
    std::vector<Holder> holders;
};

// The package's pending update against the dependencies in store (read with or without dynamic
// deps) of the packages in scope, every installed one when scope is empty. An atom holds unless
// the target matches it; a slot operator's sub-slot does not count (emerge rebuilds the
// dependent instead), nor does an alternative of a || group that another installed package
// still satisfies.
[[nodiscard]] WeighedUpdate weigh_update(const Store& store, const Graph& graph,
                                         const Evaluated& evaluated, std::uint32_t package,
                                         UseRebuilds rebuilds, const std::vector<bool>& scope = {});

// What emerge -uD would replace, as "cpv<TAB>kind<TAB>target cpv<TAB>repo": kind upgrade,
// downgrade, or rebuild when the installed package is masked and the target has its version.
// With rebuilds, each USE rebuild too, kind rebuild with "<TAB>flags" appended, the flags in
// emerge's notation, space-separated. With held, also each update dependents hold back, once per
// atom holding it: "cpv<TAB>held<TAB>target cpv<TAB>repo<TAB>dependent cpv<TAB>atom". In the
// installed packages' order.
[[nodiscard]] std::vector<std::string> update_lines(const Store& store, const Graph& graph,
                                                    const Evaluated& evaluated,
                                                    UseRebuilds rebuilds, bool held = false,
                                                    const std::vector<bool>& scope = {});

// Consumers (or providers) of a soname: one "cpv<TAB>multilib category" line each, sorted.
[[nodiscard]] std::vector<std::string> soname_users(const Store& store, std::string_view soname,
                                                    bool providers);

// A dependency as portage's paren_enclose renders it: "atom", "|| ( ... )" or "( ... )".
[[nodiscard]] std::string render(const Store& store, std::span<const Node> nodes,
                                 std::size_t index);

// A top-level dependency of a package that nothing installed satisfies.
struct Unsatisfied {
    std::uint32_t package = 0;
    // Index into dep_kinds.
    std::uint32_t kind = 0;
    // Index of the atom or group node in the package's list of that kind.
    std::uint32_t node = 0;
};

// A package's unsatisfied dependencies, in dep_kinds order, then node order.
[[nodiscard]] std::vector<Unsatisfied> unsatisfied(const Store& store, std::uint32_t package);

[[nodiscard]] std::string render(const Store& store, const Unsatisfied& dependency);

// Installed packages with the category and name of an atom in the dependency: what stands where
// it asked for another version or slot. Sorted ids; empty when nothing of the kind is installed.
[[nodiscard]] std::vector<std::uint32_t> installed_instead(const Store& store,
                                                           const Unsatisfied& dependency);

// A build-time dependency whose package is installed at another version or slot only records
// what the package was built with, not something it lacks.
[[nodiscard]] bool replaced(const Unsatisfied& dependency, std::span<const std::uint32_t> instead);

// broken's records, each with "<TAB>cpv cpv..." appended for what is installed in its place when
// anything is, split into the ones that break a package and the replaced ones.
struct BrokenRecords {
    std::vector<std::string> broken;
    std::vector<std::string> replaced;
};

[[nodiscard]] BrokenRecords broken_records(const Store& store);

// Each top-level dependency nothing installed satisfies: "cpv<TAB>kind<TAB>dependency", sorted.
[[nodiscard]] std::vector<std::string> broken(const Store& store);

enum class Direction : std::uint8_t { reverse, forward, both };

// Packages within depth dependency edges of roots, following edges in direction; sorted ids.
[[nodiscard]] std::vector<std::uint32_t> neighborhood(const Graph& graph,
                                                      std::span<const std::uint32_t> roots,
                                                      std::uint32_t depth, Direction direction);

// Graphviz of packages and the dependency edges between them, roots filled.
void write_dot(std::ostream& out, const Store& store, const Graph& graph,
               std::span<const std::uint32_t> packages, std::span<const std::uint32_t> roots);

void write_stats(std::ostream& out, const Store& store, const Graph& graph,
                 const std::filesystem::path& path);

} // namespace egraph
