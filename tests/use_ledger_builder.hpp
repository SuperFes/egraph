#pragma once

// The evaluated store's USE ledger built from lines, for tests of what stacks it.

#include "evaluated.hpp"
#include "system_builder.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace egraph::test {

using detail::Interner;

// One ledger entry: tokens are space-separated.
struct UseLine {
    std::string file = {};
    std::uint32_t line = 1;
    std::string atom = {};
    std::string var = "USE";
    std::string tokens = {};
};

// Entries by the name of one of ledger_files.
using UseFiles = std::map<std::string, std::vector<UseLine>, std::less<>>;

struct UseRepository {
    std::string name = {};
    std::vector<std::string> masters = {};
    UseFiles files = {};
};

struct UseSpec {
    std::string use_order = "env pkg conf defaults pkginternal features repo env.d";
    std::vector<std::string> use_expand = {};
    std::vector<std::string> unprefixed = {};
    std::string arch = {};
    std::vector<UseFiles> profiles = {};
    // Each profile node's directory, "/profile" past the end.
    std::vector<std::string> profile_paths = {};
    std::vector<UseRepository> repositories = {};
    std::vector<UseLine> conf = {};
    std::vector<UseLine> package_use = {};
    std::vector<UseLine> package_env = {};
    std::vector<std::pair<std::string, std::vector<UseLine>>> env_files = {};
};

// The per-package layers of a candidate.
struct OwnLayers {
    std::string cpv = {};
    bool stable = false;
    std::string internal = {};
    std::string features = {};
};

inline Range ids_of(Evaluated& ev, Interner& intern, const std::vector<std::string>& words) {
    const Range range{.first = static_cast<std::uint32_t>(ev.ids.size()),
                      .count = static_cast<std::uint32_t>(words.size())};
    for (const auto& word : words) {
        ev.ids.push_back(intern(word));
    }
    return range;
}

inline Range entries_of(Evaluated& ev, Interner& intern, const std::vector<UseLine>& lines) {
    // Tokens first: ids and entries are separate tables, but each entry's range must be final.
    std::vector<LedgerEntry> made;
    for (const auto& line : lines) {
        made.push_back({.file = intern(line.file),
                        .line = line.line,
                        .atom = intern(line.atom),
                        .var = intern(line.var),
                        .tokens = ids_of(ev, intern, detail::tokens(line.tokens))});
    }
    const Range range{.first = static_cast<std::uint32_t>(ev.ledger_entries.size()),
                      .count = static_cast<std::uint32_t>(made.size())};
    ev.ledger_entries.insert(ev.ledger_entries.end(), made.begin(), made.end());
    return range;
}

inline LedgerSources sources_of(Evaluated& ev, Interner& intern, const UseFiles& files) {
    LedgerSources sources{};
    for (std::size_t i = 0; i < ledger_files.size(); ++i) {
        const auto found = files.find(ledger_files.at(i));
        sources.at(i) =
            entries_of(ev, intern, found == files.end() ? std::vector<UseLine>{} : found->second);
    }
    return sources;
}

inline void set_ledger(Evaluated& ev, const UseSpec& spec) {
    Interner intern(ev);
    auto& ledger = ev.ledger;
    ledger.use_order = ids_of(ev, intern, detail::tokens(spec.use_order));
    ledger.use_expand = ids_of(ev, intern, spec.use_expand);
    ledger.use_expand_unprefixed = ids_of(ev, intern, spec.unprefixed);
    ledger.arch = intern(spec.arch);
    for (std::size_t i = 0; i < spec.profiles.size(); ++i) {
        const auto path = i < spec.profile_paths.size() ? spec.profile_paths.at(i) : "/profile";
        ledger.profiles.push_back(
            {.path = intern(path), .sources = sources_of(ev, intern, spec.profiles.at(i))});
    }
    for (const auto& repo : spec.repositories) {
        ledger.repositories.push_back({.name = intern(repo.name),
                                       .masters = ids_of(ev, intern, repo.masters),
                                       .sources = sources_of(ev, intern, repo.files)});
    }
    ledger.conf = entries_of(ev, intern, spec.conf);
    ledger.package_use = entries_of(ev, intern, spec.package_use);
    ledger.package_env = entries_of(ev, intern, spec.package_env);
    for (const auto& [name, lines] : spec.env_files) {
        ledger.env_files.push_back(
            {.name = intern(name), .entries = entries_of(ev, intern, lines)});
    }
}

} // namespace egraph::test
