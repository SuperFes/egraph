#include "glsa.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

namespace egraph {

namespace {

struct RevisionRange {
    std::string_view prefix;
    // Holds when the installed revision compares to the range's as order, or is equal and
    // the range inclusive.
    int order = 0;
    bool inclusive = false;

    [[nodiscard]] bool holds(int found) const { return found == 0 ? inclusive : found == order; }
};

constexpr std::array revision_ranges{
    RevisionRange{.prefix = ">=~", .order = 1, .inclusive = true},
    RevisionRange{.prefix = "<=~", .order = -1, .inclusive = true},
    RevisionRange{.prefix = ">~", .order = 1, .inclusive = false},
    RevisionRange{.prefix = "<~", .order = -1, .inclusive = false},
};

// -rN's digits as the integers Python compares: leading zeros dropped, then by length.
int compare_revisions(std::string_view a, std::string_view b) {
    const auto trim = [](std::string_view digits) {
        const auto first = digits.find_first_not_of('0');
        return first == std::string_view::npos ? std::string_view{} : digits.substr(first);
    };
    a = trim(a);
    b = trim(b);
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    const auto order = a.compare(b);
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
}

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\n") - first + 1);
}

bool arch_applies(std::string_view arches, std::string_view arch) {
    if (arches == "*") {
        return true;
    }
    return std::ranges::any_of(std::views::split(arches, ' '),
                               [&](const auto word) { return std::string_view{word} == arch; });
}

} // namespace

bool in_advisory_range(const Store& store, const Package& pkg, std::string_view range) {
    range = trimmed(range);
    for (const auto& revision : revision_ranges) {
        if (!range.starts_with(revision.prefix)) {
            continue;
        }
        auto atom = parse_atom(std::format("={}", range.substr(revision.prefix.size())));
        if (!atom || !atom->version) {
            return false;
        }
        const auto wanted = atom->version->revision;
        const auto base = parse_version(atom->version->base);
        if (!base) {
            return false;
        }
        atom->op = Operator::approximately;
        atom->version = *base;
        if (!matches(store, pkg, *atom)) {
            return false;
        }
        const auto installed =
            parse_version(store.string(pkg.cpv).substr(store.string(pkg.cp).size() + 1));
        return installed && revision.holds(compare_revisions(installed->revision, wanted));
    }
    const auto atom = parse_atom(range);
    return atom && matches(store, pkg, *atom);
}

std::vector<AffectedAdvisory> affected_advisories(const RepositoryIndex& index,
                                                  const Store& installed,
                                                  std::span<const std::string> applied) {
    std::map<std::string_view, std::vector<std::size_t>> by_cp;
    for (std::size_t i = 0; i < installed.packages.size(); ++i) {
        by_cp[installed.string(installed.packages.at(i).cp)].push_back(i);
    }
    const auto arch = index.string(index.visibility.arch);
    const std::set<std::string_view> skipped(applied.begin(), applied.end());
    std::vector<AffectedAdvisory> found;
    for (const auto& advisory : index.advisories) {
        const auto id = index.string(advisory.id);
        if (skipped.contains(id)) {
            continue;
        }
        std::map<std::string_view, std::vector<std::string>> fixed_by_cpv;
        for (const auto& entry : index.packages_in(advisory.packages)) {
            const auto packages = by_cp.find(index.string(entry.cp));
            if (packages == by_cp.end() || !arch_applies(index.string(entry.arch), arch)) {
                continue;
            }
            const auto in_any = [&](const Package& pkg, Range ranges) {
                return std::ranges::any_of(index.ids_in(ranges), [&](std::uint32_t range) {
                    return in_advisory_range(installed, pkg, index.string(range));
                });
            };
            for (const auto at : packages->second) {
                const auto& pkg = installed.packages.at(at);
                if (!in_any(pkg, entry.vulnerable) || in_any(pkg, entry.unaffected)) {
                    continue;
                }
                auto& fixed = fixed_by_cpv[installed.string(pkg.cpv)];
                for (const auto range : index.ids_in(entry.unaffected)) {
                    if (std::string text{index.string(range)};
                        !std::ranges::contains(fixed, text)) {
                        fixed.push_back(std::move(text));
                    }
                }
            }
        }
        if (fixed_by_cpv.empty()) {
            continue;
        }
        AffectedAdvisory affected{.id = std::string{id},
                                  .title = std::string{index.string(advisory.title)},
                                  .revision = advisory.revision,
                                  .packages = {}};
        for (auto& [cpv, fixed] : fixed_by_cpv) {
            affected.packages.push_back({.cpv = std::string{cpv}, .fixed = std::move(fixed)});
        }
        found.push_back(std::move(affected));
    }
    return found;
}

std::vector<std::string> applied_advisories(const std::filesystem::path& eroot) {
    std::vector<std::string> found;
    std::ifstream in{eroot / "var/lib/portage/glsa_injected"};
    for (std::string line; std::getline(in, line);) {
        // As grabfile: the words before the first that starts a comment.
        std::istringstream words{line};
        std::string kept;
        for (std::string word; words >> word && !word.starts_with('#');) {
            kept += kept.empty() ? word : " " + word;
        }
        if (!kept.empty()) {
            found.push_back(std::move(kept));
        }
    }
    return found;
}

} // namespace egraph
