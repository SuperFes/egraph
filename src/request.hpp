#pragma once

#include "evaluated.hpp"
#include "query.hpp"
#include "store.hpp"

#include <expected>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// What emerge is asked for, as its arguments.
struct Request {
    // @installed: every installed package's slot is an argument.
    bool installed = false;
    std::vector<Argument> arguments;
    // The cps of arguments that only the repositories know: the builder evaluates them before
    // they are planned. Sorted, distinct.
    std::vector<std::string> unevaluated;
    // "@set" for each set named that emerge records in world_sets unless --oneshot, sorted,
    // distinct.
    std::vector<std::string> favorites;
};

// A set the store does not hold.
struct GivenSet {
    // As emerge's arguments hold them.
    std::vector<Argument> atoms;
    // Whether emerge records it in world_sets when asked for it without --oneshot.
    bool world_candidate = false;
    bool operator==(const GivenSet&) const = default;
};

// Sets the store does not hold, by name.
using Sets = std::map<std::string, GivenSet, std::less<>>;

// The sets words name that the store does not hold, in order, each once: for the caller to give
// parse_request.
[[nodiscard]] std::vector<std::string> sets_to_load(std::span<const std::string> words);

// The arguments words name, each a set the store holds (@world, @selected, @system, @profile,
// @installed) or an atom; an atom without a category takes the one the repositories or the
// installed packages know its name in, the one outside virtual, acct-group and acct-user when
// the others are there, as emerge picks. An error for an unknown set, an invalid or ambiguous
// atom, and an atom that nothing installed and no visible ebuild matches, once evaluated. A set
// in given expands to its atoms instead, those of cps only the repositories know unevaluated;
// what none matches is the plan's to refuse, as emerge's depgraph does.

[[nodiscard]] std::expected<Request, std::string> parse_request(const Store& store,
                                                                const Evaluated& evaluated,
                                                                std::span<const std::string> words,
                                                                const Sets& given = {});

// The installed packages emerge --deep recurses into from the arguments: those the atoms match,
// or that a visible version they match would replace, and what they depend on, through the
// dependencies of the version each ends up with (an argument's best its atoms accept in its slot,
// any other package's update) and of the new packages they pull in, and through the one
// alternative of each || emerge takes: one already in the graph, else installed, else visible.
[[nodiscard]] std::vector<bool> request_reach(const Store& store, const Evaluated& evaluated,
                                              const Request& request);

} // namespace egraph
