#pragma once

// The words a shell offers for a package argument, answered from the stores.

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"

#include <cstdint>
#include <functional>
#include <optional>
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
// categories for atoms once the word is not empty, as requests fill the category in. With the
// repository index, atoms' versions and slots are every one in the repositories, and the
// repositories every one configured; without it, those the stores hold.
[[nodiscard]] std::vector<std::string>
complete_word(const Store& store, const Evaluated& evaluated,
              std::optional<std::reference_wrapper<const RepositoryIndex>> index, Completing what,
              std::string_view word);

// Whether completing word needs the index's versions, slots or repositories.
[[nodiscard]] bool needs_index(Completing what, std::string_view word);

} // namespace egraph
