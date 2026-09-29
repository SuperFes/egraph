#pragma once

#include "evaluated.hpp"
#include "store.hpp"
#include "version.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

enum class Operator : std::uint8_t {
    none,
    less,
    less_equal,
    equal,
    // =cat/pkg-1.2*
    glob,
    // ~cat/pkg-1.2: any revision
    approximately,
    greater_equal,
    greater,
};

struct UseDependency {
    std::string flag;
    // [flag] rather than [-flag].
    bool enabled = true;
    // (+) or (-): how the flag counts when the package's IUSE lacks it.
    enum class Default : std::uint8_t { none, enabled, disabled } fallback = Default::none;
};

// A dependency atom as a query argument. Blockers and conditional USE dependencies are
// rejected: both only mean something next to a parent package.
struct Atom {
    Operator op = Operator::none;
    std::string cp;
    std::optional<Version> version;
    std::optional<std::string> slot;
    std::optional<std::string> sub_slot;
    // := or :slot=, :slot/sub-slot=: emerge rebuilds the dependent against a new sub-slot.
    bool slot_operator = false;
    std::optional<std::string> repo;
    std::vector<UseDependency> use;
};

[[nodiscard]] std::expected<Atom, std::string> parse_atom(std::string_view text);

// Whether an installed package satisfies atom, as portage's vardb.match decides it.
[[nodiscard]] bool matches(const Store& store, const Package& pkg, const Atom& atom);

// Whether an ebuild, with the USE it would be built with now, satisfies atom, as depgraph matches
// an ebuild against a dependency. Implicit IUSE is the installed store's profile's, and every
// ebuild counts as having IUSE_EFFECTIVE (EAPI 5 and later).
[[nodiscard]] bool matches(const Store& installed, const Evaluated& evaluated,
                           const Candidate& candidate, const Atom& atom);

} // namespace egraph
