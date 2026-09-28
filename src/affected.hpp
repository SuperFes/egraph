#pragma once

// What portage's neighborhood completion asks of the installed packages (the fork's
// depgraph._complete_neighborhood), answered in one call so that emerge need not index them
// itself. Atoms match as the fork's InstalledGraph matches them, by version and slot alone:
// its answers are supersets, which completion needs, and shadow mode compares like with like.

#include "store.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

struct AffectedRequest {
    // Dependency kinds reachable follows: dep_kinds names, and SONAME.
    std::vector<std::string> kinds;
    // Installed cpvs to follow them from.
    std::vector<std::string> seeds;
    // cps of the packages scheduled for merge.
    std::vector<std::string> changed;
    // Installed cpvs those merges replace, whose sonames may go away.
    std::vector<std::string> replaced;
    // Blocker atoms of the packages scheduled for merge.
    std::vector<std::string> blockers;
};

struct AffectedAnswer {
    // Installed cpvs reachable from the seeds over the kinds, any-of members all followed.
    std::vector<std::string> reachable;
    // Installed cpvs a blocker matches.
    std::vector<std::string> blocked;
    // Installed cpvs whose dependencies can see a change: the blocked, packages with a
    // dependency (blockers included) naming a changed or blocked cp, and consumers of a soname a
    // replaced package provides.
    std::vector<std::string> affected;
};

// The request as JSON: an object of string lists, every key optional.
[[nodiscard]] std::expected<AffectedRequest, std::string> parse_request(std::string_view text);

// Installed packages an atom from a dependency string matches by version and slot: a blocker's
// "!" and any USE dependencies are dropped. Unparseable atoms match nothing.
[[nodiscard]] std::vector<std::uint32_t> loose_matches(const Store& store, std::string_view atom);

// Every list sorted.
[[nodiscard]] AffectedAnswer affected(const Store& store, const AffectedRequest& request);

[[nodiscard]] std::string to_json(const AffectedAnswer& answer);

} // namespace egraph
