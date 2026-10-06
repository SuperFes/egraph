#pragma once

// The words a shell offers for a package argument, answered from the stores.

#include "evaluated.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

enum class Completing : std::uint8_t {
    // Installed packages: cps, cpvs and atoms matching them.
    installed,
    // Atoms of packages in the repositories or installed, with or without their category, and
    // sets.
    atoms,
    // Repository names.
    repositories,
};

// What word may grow into, sorted: a category with its "/" until the word has one, then cps
// (installed cpvs once the word runs past its cp); an atom's versions after an operator, its
// slots after ":", repositories after "::"; sets after "@". Names without a category join the
// categories for atoms once the word is not empty, as requests fill the category in.
[[nodiscard]] std::vector<std::string> complete_word(const Store& store, const Evaluated& evaluated,
                                                     Completing what, std::string_view word);

} // namespace egraph
