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
};

// The arguments words name, each a set the store holds (@world, @selected, @system, @profile,
// @installed) or an atom; an atom without a category takes the one the repositories or the
// installed packages know its name in, the one outside virtual, acct-group and acct-user when
// the others are there, as emerge picks. An error for an unknown set, an invalid or ambiguous
// atom, and an atom that nothing installed and no visible ebuild matches, once evaluated. A set
// in given expands to its atoms instead.
// Sets the store does not hold, by name, each with its atoms.
using Sets = std::map<std::string, std::vector<std::string>, std::less<>>;

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
