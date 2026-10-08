#pragma once

// Repository indexes built in memory, for tests of what reads one.

#include "repository.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace egraph::test {

inline std::vector<std::string> words(std::string_view text) {
    std::istringstream in{std::string{text}};
    std::vector<std::string> found;
    for (std::string word; in >> word;) {
        found.push_back(word);
    }
    return found;
}

struct VersionSpec {
    std::string cpv;
    std::string keywords = "x86";
    std::string slot = "0";
    std::string license = {};
    std::string properties = {};
    std::string restrict = {};
    std::string use = {};
    std::string eapi = "8";
    std::string repo = "gentoo";
    std::string description = {};
    std::string homepage = {};
};

// A line of the visibility ledger.
struct LedgerLine {
    std::string atom = {};
    std::string var = {};
    std::string tokens = {};
    std::string file = "file";
    std::uint32_t line = 1;
};

// A repository index on x86, ACCEPT_KEYWORDS="x86" and everything else accepted unless a test
// says otherwise.
class IndexBuilder {
  public:
    // Repositories by priority, highest first.
    explicit IndexBuilder(std::vector<std::string> repositories = {"gentoo", "overlay"})
        : names_{std::move(repositories)} {
        intern("");
        for (const auto& name : names_) {
            index_.repositories.push_back({.name = intern(name), .location = intern("/")});
        }
        auto& vis = index_.visibility;
        vis.accept_keywords = ids("x86");
        vis.arch = intern("x86");
        for (const auto* eapi : {"7", "8"}) {
            vis.eapis.push_back({.eapi = intern(eapi), .supported = true, .deprecated = false});
        }
        vis.eapis.push_back({.eapi = intern("6"), .supported = true, .deprecated = true});
        vis.eapis.push_back({.eapi = intern("99"), .supported = false, .deprecated = false});
        vis.accept_license = ids("*");
        vis.accept_properties = ids("*");
        vis.accept_restrict = ids("*");
    }

    std::uint32_t version(const VersionSpec& spec) {
        egraph::IndexVersion version;
        const auto dash = spec.cpv.rfind('-', spec.cpv.find_last_of("0123456789") - 1);
        version.cp = intern(spec.cpv.substr(0, dash));
        version.cpv = intern(spec.cpv);
        const auto slash = spec.slot.find('/');
        version.slot = intern(spec.slot.substr(0, slash));
        version.sub_slot =
            intern(slash == std::string::npos ? spec.slot : spec.slot.substr(slash + 1));
        version.eapi = intern(spec.eapi);
        version.repository =
            static_cast<std::uint32_t>(std::ranges::find(names_, spec.repo) - names_.begin());
        version.keywords = ids(spec.keywords);
        version.license = ids(spec.license);
        version.properties = ids(spec.properties);
        version.restrict = ids(spec.restrict);
        version.use = ids(spec.use);
        version.description = intern(spec.description);
        version.homepage = intern(spec.homepage);
        index_.versions.push_back(version);
        return static_cast<std::uint32_t>(index_.versions.size() - 1);
    }

    Range entries(const std::vector<std::pair<std::string, std::string>>& lines) {
        const auto first = static_cast<std::uint32_t>(index_.entries.size());
        for (const auto& [atom, tokens] : lines) {
            index_.entries.push_back({.atom = intern(atom), .tokens = ids(tokens)});
        }
        return {.first = first, .count = static_cast<std::uint32_t>(index_.entries.size()) - first};
    }

    // Ledger entries, appended in order.
    Range lines(const std::vector<LedgerLine>& lines) {
        const auto first = static_cast<std::uint32_t>(index_.ledger_entries.size());
        for (const auto& line : lines) {
            index_.ledger_entries.push_back({.file = intern(line.file),
                                             .line = line.line,
                                             .atom = intern(line.atom),
                                             .var = intern(line.var),
                                             .tokens = ids(line.tokens)});
        }
        return {.first = first,
                .count = static_cast<std::uint32_t>(index_.ledger_entries.size()) - first};
    }

    Range ids(std::string_view text) {
        const auto first = static_cast<std::uint32_t>(index_.ids.size());
        for (const auto& word : words(text)) {
            index_.ids.push_back(intern(word));
        }
        return {.first = first, .count = static_cast<std::uint32_t>(index_.ids.size()) - first};
    }

    // A GLSA; packages are (cp, arch, vulnerable atoms, unaffected atoms), atoms space-separated.
    void advisory(std::string_view id, std::string_view title,
                  const std::vector<std::array<std::string, 4>>& packages,
                  std::uint64_t revision = 1) {
        egraph::Advisory advisory;
        advisory.id = intern(id);
        advisory.title = intern(title);
        advisory.synopsis = intern("");
        advisory.revision = revision;
        advisory.packages.first = static_cast<std::uint32_t>(index_.advisory_packages.size());
        for (const auto& [cp, arch, vulnerable, unaffected] : packages) {
            index_.advisory_packages.push_back({.cp = intern(cp),
                                                .arch = intern(arch),
                                                .vulnerable = ids(vulnerable),
                                                .unaffected = ids(unaffected)});
        }
        advisory.packages.count =
            static_cast<std::uint32_t>(index_.advisory_packages.size()) - advisory.packages.first;
        index_.advisories.push_back(advisory);
    }

    egraph::VisibilityConfig& config() { return index_.visibility; }
    egraph::VisibilityLedger& ledger() { return index_.ledger; }
    std::uint32_t string(std::string_view text) { return intern(text); }
    void locate(std::string_view repository, std::string_view location) {
        const auto at = std::ranges::find(names_, repository) - names_.begin();
        index_.repositories.at(static_cast<std::size_t>(at)).location = intern(location);
    }
    // The repository's descriptions come from a metadata/pkg_desc_index.
    void describe(std::string_view repository) {
        const auto at = std::ranges::find(names_, repository) - names_.begin();
        index_.repositories.at(static_cast<std::size_t>(at)).description_index = true;
    }
    [[nodiscard]] const RepositoryIndex& index() const { return index_; }

  private:
    std::uint32_t intern(std::string_view text) {
        const auto [found, added] =
            ids_.emplace(std::string{text}, static_cast<std::uint32_t>(index_.strings.size()));
        if (added) {
            index_.strings.push_back({.first = static_cast<std::uint32_t>(index_.pool.size()),
                                      .count = static_cast<std::uint32_t>(text.size())});
            index_.pool += text;
        }
        return found->second;
    }

    std::vector<std::string> names_;
    RepositoryIndex index_;
    std::map<std::string, std::uint32_t, std::less<>> ids_;
};

} // namespace egraph::test
