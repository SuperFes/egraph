#include "config_check.hpp"

#include "atom.hpp"
#include "version.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <tuple>

namespace egraph {

std::string_view kind_name(FindingKind kind) {
    switch (kind) {
    case FindingKind::dead:
        return "dead";
    case FindingKind::contradicted:
        return "contradicted";
    case FindingKind::no_effect:
        return "no-effect";
    case FindingKind::not_installed:
        return "not-installed";
    }
    return {};
}

std::string_view severity_name(FindingKind kind) {
    switch (kind) {
    case FindingKind::dead:
    case FindingKind::contradicted:
        return "error";
    case FindingKind::no_effect:
        return "warning";
    case FindingKind::not_installed:
        return "note";
    }
    return {};
}

void order_findings(std::vector<Finding>& findings) {
    std::ranges::stable_sort(findings, {}, [](const Finding& each) {
        return std::tie(each.kind, each.file, each.line);
    });
}

bool failing(std::span<const Finding> findings) {
    return std::ranges::any_of(findings, [](const Finding& each) {
        return each.kind == FindingKind::dead || each.kind == FindingKind::contradicted;
    });
}

std::string finding_record(const Finding& finding) {
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}", finding.file, finding.line,
                       severity_name(finding.kind), kind_name(finding.kind), finding.atom,
                       finding.token, finding.message);
}

namespace {

void add_entries(std::vector<UserEntry>& found, const Tables& tables,
                 std::span<const LedgerEntry> entries) {
    for (const auto& entry : entries) {
        if (entry.atom != 0) {
            found.push_back({.file = std::string{tables.string(entry.file)},
                             .line = entry.line,
                             .atom = std::string{tables.string(entry.atom)}});
        }
    }
}

// What a cp's versions in the repositories and installed packages are, by index.
struct Known {
    std::vector<std::uint32_t> versions;
    std::vector<std::uint32_t> installed;
};

std::optional<Version> version_in(std::string_view cpv, std::string_view cp) {
    return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
}

// The user's own profile node, as portage adds it after the profiles.
bool user_profile(std::string_view path) {
    return path.ends_with("/etc/portage/profile") || path.ends_with("/etc/portage/profile/");
}

} // namespace

std::vector<UserEntry> user_entries(const Evaluated& evaluated, const RepositoryIndex& index) {
    std::vector<UserEntry> found;
    const auto& use = evaluated.ledger;
    for (const auto range : {use.conf, use.package_use, use.package_env}) {
        add_entries(found, evaluated, evaluated.entries_in(range));
    }
    for (const auto& node : use.profiles) {
        if (user_profile(evaluated.string(node.path))) {
            for (const auto range : node.sources) {
                add_entries(found, evaluated, evaluated.entries_in(range));
            }
        }
    }
    const auto& visibility = index.ledger;
    for (const auto& node : visibility.profiles) {
        if (user_profile(index.string(node.path))) {
            for (const auto range : {node.package_mask, node.package_unmask, node.package_keywords,
                                     node.package_accept_keywords, node.package_license}) {
                add_entries(found, index, index.ledger_entries_in(range));
            }
        }
    }
    for (const auto range : {visibility.conf, visibility.package_mask, visibility.package_unmask,
                             visibility.package_keywords, visibility.package_accept_keywords,
                             visibility.package_license, visibility.package_properties,
                             visibility.package_accept_restrict}) {
        add_entries(found, index, index.ledger_entries_in(range));
    }
    // A USE_EXPAND line is an entry per variable.
    const auto key = [](const UserEntry& each) {
        return std::tie(each.file, each.line, each.atom);
    };
    std::ranges::sort(found, {}, key);
    const auto [first, last] = std::ranges::unique(found, {}, key);
    found.erase(first, last);
    return found;
}

std::vector<Finding> unmatched_entries(std::span<const UserEntry> entries, const Store& installed,
                                       const RepositoryIndex& index) {
    std::map<std::string, Known, std::less<>> known;
    for (std::uint32_t id = 0; id < index.versions.size(); ++id) {
        known[std::string{index.string(index.versions.at(id).cp)}].versions.push_back(id);
    }
    for (std::uint32_t id = 0; id < installed.packages.size(); ++id) {
        known[std::string{installed.string(installed.packages.at(id).cp)}].installed.push_back(id);
    }
    // Whether atom matches any of cp's, and any installed.
    const auto matching = [&](const Atom& atom, const std::string& cp, const Known& each) {
        std::pair<bool, bool> found{false, !each.installed.empty()};
        for (const auto id : each.versions) {
            const auto& version = index.versions.at(id);
            const auto parsed = version_in(index.string(version.cpv), cp);
            if (parsed && matches(atom, cp, *parsed, index.string(version.slot),
                                  index.string(version.sub_slot),
                                  index.string(index.repositories.at(version.repository).name))) {
                found.first = true;
                return found;
            }
        }
        for (const auto id : each.installed) {
            const auto& package = installed.packages.at(id);
            const auto parsed = version_in(installed.string(package.cpv), cp);
            if (parsed &&
                matches(atom, cp, *parsed, installed.string(package.slot),
                        installed.string(package.sub_slot), installed.string(package.repo))) {
                found.first = true;
                return found;
            }
        }
        return found;
    };
    std::vector<Finding> found;
    for (const auto& entry : entries) {
        const std::string_view text = entry.atom;
        const auto atom = parse_config_atom(text.starts_with('-') ? text.substr(1) : text);
        const auto finding = [&](FindingKind kind, std::string message) {
            found.push_back({.kind = kind,
                             .file = entry.file,
                             .line = entry.line,
                             .atom = entry.atom,
                             .message = std::move(message)});
        };
        if (!atom) {
            finding(FindingKind::dead, "is not an atom portage reads");
            continue;
        }
        bool matched = false;
        bool installed_cp = false;
        const auto visit = [&](const std::string& cp, const Known& each) {
            if (const auto [hit, cp_installed] = matching(*atom, cp, each); hit) {
                matched = true;
                installed_cp = installed_cp || cp_installed;
            }
        };
        if (atom->cp.contains('*')) {
            for (const auto& [cp, each] : known) {
                visit(cp, each);
            }
        } else if (const auto hit = known.find(atom->cp); hit != known.end()) {
            visit(hit->first, hit->second);
        }
        if (!matched) {
            finding(FindingKind::dead, "matches nothing in any repository");
        } else if (!installed_cp) {
            finding(FindingKind::not_installed, "matches only packages not installed");
        }
    }
    return found;
}

std::vector<Finding> check_config(const Store& installed, const Evaluated& evaluated,
                                  const RepositoryIndex& index) {
    const auto entries = user_entries(evaluated, index);
    auto found = unmatched_entries(entries, installed, index);
    order_findings(found);
    return found;
}

} // namespace egraph
