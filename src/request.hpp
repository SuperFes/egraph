#pragma once

#include "evaluated.hpp"
#include "query.hpp"
#include "store.hpp"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// What emerge is asked for, as its arguments.
struct Request {
    // @installed: every installed package's slot is an argument.
    bool installed = false;
    std::vector<Argument> arguments;
};

// The arguments words name, each a set the store holds (@world, @selected, @system, @profile,
// @installed) or an atom; an atom without a category takes the one the stores know its name in.
// An error for an unknown set, an invalid or ambiguous atom, and an atom that nothing installed
// and no visible ebuild matches.
[[nodiscard]] std::expected<Request, std::string>
parse_request(const Store& store, const Evaluated& evaluated, std::span<const std::string> words);

// The installed packages emerge --deep recurses into from the arguments: those the atoms match,
// or that a visible version they match would replace, and what they depend on, through the
// installed packages' dependencies and those of the versions they would move to, starting from
// the visible versions the atoms match as well.
[[nodiscard]] std::vector<bool> request_reach(const Store& store, const Evaluated& evaluated,
                                              const Request& request);

} // namespace egraph
