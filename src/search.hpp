#pragma once

// emerge --search over the repository index and the installed packages.

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"
#include "visibility.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
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

// What emerge --search shows for a cp.
struct Found {
    std::string cp;
    // The best visible version among the repositories' and the installed ones, else the best of
    // all, as portage's getVersion words it (no "-r0").
    std::string version;
    bool visible = false;
    // The latest installed, empty for none.
    std::string installed;
    // The chosen version's ebuild's, from the repository of highest priority holding it; empty
    // for a version only installed.
    std::string homepage;
    std::string license;
    std::string description;
};

// One version of a cp: an ebuild in a repository, or an installed package no repository holds.
struct PackageVersion {
    // As the cpv has it, revision and all.
    std::string version;
    std::string slot;
    std::string sub_slot;
    std::string repo;
    // Whether a repository holds it.
    bool ebuild = true;
    bool visible = false;
    // As VersionMasks::reasons, for an ebuild.
    std::vector<std::string> reasons;
    // Into the installed store, for the installed package of this cpv from this repository.
    std::optional<std::uint32_t> installed;
};

// The packages emerge --search looks through: the repository index's versions and the installed
// ones, by cp. Without an evaluated store paired with the installed one, every installed package
// counts as visible.
class Catalogue {
  public:
    Catalogue(const Store& installed EGRAPH_LIFETIMEBOUND,
              const Evaluated& evaluated EGRAPH_LIFETIMEBOUND,
              const RepositoryIndex& index EGRAPH_LIFETIMEBOUND,
              const VersionMasks& masks EGRAPH_LIFETIMEBOUND);

    // Sorted and distinct.
    [[nodiscard]] std::span<const std::string_view> cps() const EGRAPH_LIFETIMEBOUND {
        return cps_;
    }
    [[nodiscard]] bool contains(std::string_view cp) const { return by_cp_.contains(cp); }
    // The DESCRIPTION emerge --search -S matches; empty for a cp only installed.
    [[nodiscard]] std::string_view description(std::string_view cp) const EGRAPH_LIFETIMEBOUND;
    // search_cps over cps() and description().
    [[nodiscard]] std::expected<std::vector<std::string_view>, std::string>
    search(std::string_view key, const SearchOptions& options) const EGRAPH_LIFETIMEBOUND;
    // For a cp of cps().
    [[nodiscard]] Found found(std::string_view cp) const;
    // A cp's versions, lowest first; one version's ebuilds by repository priority, then any
    // installed package of it from a repository the index does not hold it in.
    [[nodiscard]] std::vector<PackageVersion> versions(std::string_view cp) const;

  private:
    struct Versions {
        std::vector<std::uint32_t> ebuilds;
        std::vector<std::uint32_t> installed;
    };

    [[nodiscard]] bool installed_visible(std::uint32_t package) const;

    std::reference_wrapper<const Store> installed_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::reference_wrapper<const RepositoryIndex> index_;
    std::reference_wrapper<const VersionMasks> masks_;
    std::map<std::string_view, Versions, std::less<>> by_cp_;
    std::vector<std::string_view> cps_;
};

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
