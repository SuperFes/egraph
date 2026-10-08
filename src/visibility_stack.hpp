#pragma once

// The visibility ledger stacked as portage's managers stack it (MaskManager, KeywordsManager,
// LicenseManager, config's ACCEPT_ variables and package.properties and package.accept_restrict),
// every stacked atom and token with the ledger entry it came from.

#include "repository.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace egraph {

struct SourcedToken {
    std::string token;
    // Index into RepositoryIndex::ledger_entries; none for portage's built-in ACCEPT_LICENSE.
    std::optional<std::uint32_t> entry;
};

// A package.* key as a manager's {cp: {atom: tokens}} holds it: the tokens of every entry for
// its atom within the source that kept it, a later source's replacing an earlier one's.
struct StackedKey {
    std::string atom;
    std::vector<SourcedToken> tokens;
    // The entries its tokens came from, in order.
    std::vector<std::uint32_t> entries;
};

struct StackedMask {
    // With ::repo for a repository's own mask, as append_repo makes it.
    std::string atom;
    // The last entry that listed it.
    std::uint32_t entry = 0;
};

// Lists of keys are in the managers' order: by cp, the cps as first seen, then the wildcard
// ones. Token lists are in the order they apply in.
struct StackedVisibility {
    // ACCEPT_KEYWORDS, stacked incrementally over every layer; each the entry that added it.
    std::vector<SourcedToken> accept_keywords;
    // The environment's ACCEPT_KEYWORDS.
    std::vector<SourcedToken> environment_keywords;
    // Per profile node, its package.keywords and package.accept_keywords.
    std::vector<std::vector<StackedKey>> profile_keywords;
    std::vector<std::vector<StackedKey>> profile_accept_keywords;
    // The user's package.keywords and package.accept_keywords merged, an empty key given the ~
    // form of each stable keyword the profiles' make.defaults accept.
    std::vector<StackedKey> accept_keywords_entries;
    std::vector<StackedMask> masks;
    std::vector<StackedMask> unmasks;
    // Pruned at the last * or -*, license groups expanded.
    std::vector<SourcedToken> accept_license;
    // The profiles' (profile-license) and the user's package.license, groups expanded.
    std::vector<StackedKey> licenses;
    std::vector<SourcedToken> accept_properties;
    std::vector<StackedKey> properties;
    std::vector<SourcedToken> accept_restrict;
    std::vector<StackedKey> restrict;
};

[[nodiscard]] StackedVisibility stack_visibility(const RepositoryIndex& index);

} // namespace egraph
