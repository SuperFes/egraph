#pragma once

#include "depclean.hpp"
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

// By the versions of cp's cpvs from and to.
[[nodiscard]] UpdateKind update_kind(std::string_view cp, std::string_view from,
                                     std::string_view to);

// The package's pending update, if emerge -u would replace it or, with rebuilds, rebuild it for
// USE.
[[nodiscard]] std::optional<PendingUpdate>
pending_update(const Evaluated& evaluated, std::uint32_t package, UseRebuilds rebuilds);

// Indices into Evaluated::candidates that the package's wanted update falls back to when its
// target is rejected: the other visible versions in the target's slot, newer than the installed
// one for an upgrade, best first and of one version the target's repository first. None for a
// rebuild.
[[nodiscard]] std::vector<std::uint32_t>
fallbacks(const Evaluated& evaluated, std::uint32_t package, const PendingUpdate& wanted);

// What emerge -uD would merge (plan_updates), as "cpv<TAB>kind<TAB>target cpv<TAB>repo": kind
// upgrade, downgrade, or rebuild when the installed package is masked and the target has its
// version. With rebuilds, each USE rebuild too, kind rebuild with "<TAB>flags" appended, the flags
// in emerge's notation, space-separated. With held, also each update held back or fallen back,
// once: "cpv<TAB>held<TAB>target cpv<TAB>repo<TAB>flags", the flags a held USE rebuild is for
// (else empty), then a field per package whose dependencies reject it (a dependent, or the
// target or what it would pull in): its cpv, then each of its atoms that do, space-separated. In
// the installed packages' order; then each package new in its slot,
// "cpv<TAB>new<TAB>cpv<TAB>repo<TAB>puller atom", by cpv, with the package and atom that pull it
// in. A slot-operator rebuild is kind rebuild with empty flags, then "<TAB>merge atom": the merge
// that breaks its binding and the bound atom. With table, the merges in merge order instead, each
// line led by "place<TAB>waits<TAB>", its place from 1 and the places it waits for,
// space-separated; held lines follow, led by two empty fields.
[[nodiscard]] std::vector<std::string> update_lines(const Store& store, const Evaluated& evaluated,
                                                    UseRebuilds rebuilds, bool held = false,
                                                    bool table = false,
                                                    const std::vector<bool>& scope = {});

// Where each merge of the plan comes from, in merge order: "place<TAB>@set<TAB>cpv<TAB>...", its
// place as update_lines' table numbers it, then the root set and the chain of installed packages
// why finds from the set's atom down to the one the merge replaces (or rebuilds). A new package's
// chain is that of the member that pulled it in, then its own cpv. A merge nothing keeps has an
// empty set and its own cpv only.
[[nodiscard]] std::vector<std::string> update_tree_lines(const Store& store,
                                                         const Evaluated& evaluated,
                                                         const Kept& kept, UseRebuilds rebuilds);

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
