#pragma once

// emerge --depclean with arguments: of the installed packages they match, those nothing kept needs
// are removed.

#include "depclean.hpp"
#include "store.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

struct Removal {
    // In cpv order.
    std::vector<std::uint32_t> removed;
    // A matched package depclean keeps, with what keeps it.
    struct Held {
        std::uint32_t package = 0;
        // Kept packages depending on it, in cpv order.
        std::vector<std::uint32_t> dependents;
        // Root sets keeping it, "@system" say.
        std::vector<std::string> sets;
    };
    // In cpv order.
    std::vector<Held> kept;
};

// What emerge --depclean removes of matched, given options as for keep(): depclean then keeps
// every installed package not matched, and leaves out @selected, whose atoms it deselects.
[[nodiscard]] Removal plan_removal(const Store& store, KeepOptions options,
                                   std::span<const std::uint32_t> matched);

// "cpv<TAB>remove" per removed package, then "cpv<TAB>kept<TAB>by..." per kept one, each set and
// dependent cpv a field.
[[nodiscard]] std::vector<std::string> removal_lines(const Store& store, const Removal& removal);

// The cpvs emerge --pretend --depclean would remove, from its "All selected packages:" line,
// sorted.
[[nodiscard]] std::vector<std::string> parse_depclean(std::string_view output);

// "cpv<TAB>egraph<TAB>remove" for a package only we remove, "cpv<TAB>emerge<TAB>remove" for one
// only emerge does, by cpv.
[[nodiscard]] std::vector<std::string> removal_differences(const Store& store,
                                                           const Removal& removal,
                                                           std::span<const std::string> emerge);

// emerge's options for a depclean of targets, before the targets: --with-bdeps=n without
// build_deps, --dynamic-deps=n without dynamic.
[[nodiscard]] std::vector<std::string> depclean_options(bool build_deps, bool dynamic);

} // namespace egraph
