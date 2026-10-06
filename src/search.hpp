#pragma once

// emerge --search over the repository index and the installed packages.

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"
#include "visibility.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// emerge's search options, with its defaults.
struct SearchOptions {
    // --searchdesc: a key matching a package's DESCRIPTION finds it too.
    bool description = false;
    // --fuzzy-search.
    bool fuzzy = true;
    // --regex-search-auto: a key that looks like a regular expression is one.
    bool regex_auto = true;
    // --search-similarity: how alike, in percent, a fuzzy match must be.
    std::uint32_t similarity = 80;
};

// difflib.SequenceMatcher(None, a, b).ratio(), but for its junk heuristic, which only a b of 200
// characters or more takes.
[[nodiscard]] double sequence_ratio(std::string_view a, std::string_view b);

// What emerge --search finds for key among cps (sorted and distinct), as its _iter_search finds
// packages: each cp whose name (its whole cp with a "/" in the key or a leading "@") the key
// matches, ignoring case, as a regular expression (after a leading "%", or with regex_auto one
// that looks like one and compiles), else as text anywhere or fuzzily; with description, also
// each whose description (as description gives it, empty for none) the key matches. A key after
// "%" that does not compile is the error.
[[nodiscard]] std::expected<std::vector<std::string_view>, std::string>
search_cps(std::span<const std::string_view> cps,
           const std::function<std::string_view(std::string_view)>& description,
           std::string_view key, const SearchOptions& options);

// emerge --search's findings for each key, a line each: "key<TAB>cp<TAB>version<TAB>visible" or
// masked, then the latest version installed (empty for none), the homepage, the license and the
// description. The version is the best visible one among the repositories' and the installed
// packages, else the best of all; the rest is its ebuild's, from the repository of highest
// priority holding it, empty for a version only installed.
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
search_lines(const Store& installed, const Evaluated& evaluated, const RepositoryIndex& index,
             const VersionMasks& masks, std::span<const std::string> keys,
             const SearchOptions& options);

} // namespace egraph
