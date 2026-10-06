#pragma once

// Repository indexes built in memory, for tests of what reads one.

#include "repository.hpp"

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

// A repository index on x86, ACCEPT_KEYWORDS="x86" and everything else accepted unless a test
// says otherwise.
class IndexBuilder {
  public:
    IndexBuilder() {
        intern("");
        for (const auto* name : {"gentoo", "overlay"}) {
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
        version.repository = spec.repo == "gentoo" ? 0 : 1;
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

    Range ids(std::string_view text) {
        const auto first = static_cast<std::uint32_t>(index_.ids.size());
        for (const auto& word : words(text)) {
            index_.ids.push_back(intern(word));
        }
        return {.first = first, .count = static_cast<std::uint32_t>(index_.ids.size()) - first};
    }

    egraph::VisibilityConfig& config() { return index_.visibility; }
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

    RepositoryIndex index_;
    std::map<std::string, std::uint32_t, std::less<>> ids_;
};

} // namespace egraph::test
