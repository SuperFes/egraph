#include "config_check.hpp"

#include "atom.hpp"
#include "use_stack.hpp"
#include "version.hpp"
#include "visibility.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

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

// The user's package.use entries, as indices into Evaluated::ledger_entries.
std::vector<std::uint32_t> user_use_entries(const Evaluated& evaluated) {
    std::vector<std::uint32_t> found;
    const auto take = [&](Range range) {
        for (std::uint32_t i = 0; i < range.count; ++i) {
            const auto& entry = evaluated.ledger_entries.at(range.first + i);
            if (entry.atom != 0 && evaluated.string(entry.var) == "USE") {
                found.push_back(range.first + i);
            }
        }
    };
    take(evaluated.ledger.conf);
    take(evaluated.ledger.package_use);
    for (const auto& node : evaluated.ledger.profiles) {
        if (user_profile(evaluated.string(node.path))) {
            for (const auto range : node.sources) {
                take(range);
            }
        }
    }
    return found;
}

// What set a step, for a finding on a line of file: "line N" within it, "file:line" in another,
// else the layer.
std::string step_source(const Evaluated& evaluated, const UseStep& step, std::string_view file) {
    if (step.entry) {
        const auto& entry = evaluated.ledger_entries.at(*step.entry);
        const auto where = evaluated.string(entry.file);
        if (where == file && entry.line != 0) {
            return std::format("line {}", entry.line);
        }
        if (!where.empty()) {
            return entry.line == 0 ? std::string{where} : std::format("{}:{}", where, entry.line);
        }
    }
    switch (step.layer) {
    case UseLayer::pkginternal:
        return "its IUSE default";
    case UseLayer::env:
        return "the environment";
    case UseLayer::arch:
        return "ARCH";
    default:
        return std::string{layer_name(step.layer)};
    }
}

// Where a ledger entry is: "line N" in the same file, "file:line" in another.
std::string entry_place(const RepositoryIndex& index, std::uint32_t id, std::string_view file) {
    const auto& entry = index.ledger_entries.at(id);
    const auto where = index.string(entry.file);
    if (where == file && entry.line != 0) {
        return std::format("line {}", entry.line);
    }
    return entry.line == 0 ? std::string{where} : std::format("{}:{}", where, entry.line);
}

// What a user visibility line sets.
enum class Sets : std::uint8_t { keywords, mask, unmask, license, properties, restrict };

// The user's visibility lines naming packages, as indices into RepositoryIndex::ledger_entries.
std::vector<std::pair<std::uint32_t, Sets>> user_visibility_entries(const RepositoryIndex& index) {
    std::vector<std::pair<std::uint32_t, Sets>> found;
    const auto take = [&](Range range, Sets sets) {
        for (std::uint32_t i = 0; i < range.count; ++i) {
            if (index.ledger_entries.at(range.first + i).atom != 0) {
                found.emplace_back(range.first + i, sets);
            }
        }
    };
    const auto& ledger = index.ledger;
    for (std::uint32_t i = 0; i < ledger.conf.count; ++i) {
        const auto& entry = index.ledger_entries.at(ledger.conf.first + i);
        const auto var = index.string(entry.var);
        if (entry.atom == 0) {
            continue;
        }
        if (var == "ACCEPT_LICENSE") {
            found.emplace_back(ledger.conf.first + i, Sets::license);
        } else if (var == "ACCEPT_PROPERTIES") {
            found.emplace_back(ledger.conf.first + i, Sets::properties);
        } else if (var == "ACCEPT_RESTRICT") {
            found.emplace_back(ledger.conf.first + i, Sets::restrict);
        }
    }
    for (const auto& node : ledger.profiles) {
        if (user_profile(index.string(node.path))) {
            take(node.package_mask, Sets::mask);
            take(node.package_unmask, Sets::unmask);
            take(node.package_keywords, Sets::keywords);
            take(node.package_accept_keywords, Sets::keywords);
            take(node.package_license, Sets::license);
        }
    }
    take(ledger.package_mask, Sets::mask);
    take(ledger.package_unmask, Sets::unmask);
    take(ledger.package_keywords, Sets::keywords);
    take(ledger.package_accept_keywords, Sets::keywords);
    take(ledger.package_license, Sets::license);
    take(ledger.package_properties, Sets::properties);
    take(ledger.package_accept_restrict, Sets::restrict);
    return found;
}

// Whether a masking reason is of what a line sets.
bool reason_of(Sets sets, std::string_view reason) {
    switch (sets) {
    case Sets::keywords:
        return reason.ends_with("keyword");
    case Sets::mask:
    case Sets::unmask:
        return reason == "package.mask";
    case Sets::license:
        return reason.ends_with(" license(s)");
    case Sets::properties:
        return reason.ends_with(" properties");
    case Sets::restrict:
        return reason.ends_with(" in RESTRICT");
    }
    return false;
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

std::vector<Finding> use_findings(const Store& installed, const Evaluated& evaluated) {
    const UseStacker stacker(installed, evaluated);
    std::unordered_map<std::string, std::vector<std::uint32_t>> by_cp;
    for (std::uint32_t id = 0; id < evaluated.candidates.size(); ++id) {
        by_cp[std::string{evaluated.string(evaluated.candidates.at(id).cp)}].push_back(id);
    }
    std::unordered_map<std::uint32_t, StackedUse> stacked;
    const auto stack_of = [&](std::uint32_t id) -> const StackedUse& {
        auto found = stacked.find(id);
        if (found == stacked.end()) {
            found = stacked.emplace(id, stacker.stack(evaluated.candidates.at(id))).first;
        }
        return found->second;
    };
    std::vector<Finding> found;
    for (const auto index : user_use_entries(evaluated)) {
        const auto& entry = evaluated.ledger_entries.at(index);
        const auto text = evaluated.string(entry.atom);
        const auto atom = parse_config_atom(text);
        if (!atom) {
            continue;
        }
        std::vector<std::uint32_t> matched;
        const auto visit = [&](const std::vector<std::uint32_t>& ids) {
            for (const auto id : ids) {
                const auto& candidate = evaluated.candidates.at(id);
                const auto cp = evaluated.string(candidate.cp);
                const auto cpv = evaluated.string(candidate.cpv);
                const auto version = version_in(cpv, cp);
                if (version && matches(*atom, cp, *version, evaluated.string(candidate.slot),
                                       evaluated.string(candidate.sub_slot),
                                       evaluated.string(candidate.repo))) {
                    matched.push_back(id);
                }
            }
        };
        if (atom->cp.contains('*')) {
            for (const auto& [cp, ids] : by_cp) {
                visit(ids);
            }
        } else if (const auto hit = by_cp.find(atom->cp); hit != by_cp.end()) {
            visit(hit->second);
        }
        if (matched.empty()) {
            continue;
        }
        const auto file = evaluated.string(entry.file);
        std::uint32_t position = 0;
        for (const auto id : evaluated.ids_in(entry.tokens)) {
            const auto omitted = UseStacker::Omitted{.entry = index, .position = position++};
            const std::string token{evaluated.string(id)};
            if (token.contains('*')) {
                continue;
            }
            const auto flag = token.starts_with('-') ? token.substr(1) : token;
            // Over the ebuilds it matches whose IUSE has the flag: whether it changes anything for
            // one, and else why not, overridden where it stands for each (contradicted) or not.
            bool inside = false;
            std::optional<std::string> already;
            std::optional<std::string> overridden;
            bool effective = false;
            for (const auto candidate_id : matched) {
                const auto& candidate = evaluated.candidates.at(candidate_id);
                if (!has_flag(installed, evaluated, candidate, flag)) {
                    continue;
                }
                const auto& with = stack_of(candidate_id);
                const auto steps = with.steps.find(flag);
                if (steps == with.steps.end()) {
                    continue;
                }
                const auto& list = steps->second;
                const auto own = std::ranges::find_last_if(list, [&](const UseStep& step) {
                                     return step.entry == index;
                                 }).begin();
                if (own == list.end()) {
                    continue;
                }
                inside = true;
                if (own->changed) {
                    const auto without = stacker.stack(candidate, omitted);
                    const auto state = [&](const StackedUse& use) {
                        return std::pair{std::ranges::binary_search(use.use, flag),
                                         std::ranges::binary_search(use.forced, flag)};
                    };
                    if (state(with) != state(without)) {
                        effective = true;
                        break;
                    }
                }
                if (list.back().enabled != own->enabled) {
                    if (!overridden) {
                        const auto later =
                            std::find_if(own + 1, list.end(), [&](const UseStep& step) {
                                return step.enabled != own->enabled;
                            });
                        overridden = std::format("{} overrides it for everything it matches",
                                                 step_source(evaluated, *later, file));
                    }
                } else if (!already && !own->changed) {
                    const auto before = std::ranges::find_last_if(
                        list.begin(), own, [&](const UseStep& step) { return step.changed; });
                    already = std::format(
                        "already {}{}", own->enabled ? "on" : "off",
                        before.begin() == own
                            ? std::string{}
                            : std::format(" ({})", step_source(evaluated, *before.begin(), file)));
                } else if (!already) {
                    const auto again = std::find_if(own + 1, list.end(), [&](const UseStep& step) {
                        return step.enabled == own->enabled;
                    });
                    already =
                        again == list.end()
                            ? std::string{"changes nothing"}
                            : std::format("{} sets it too", step_source(evaluated, *again, file));
                }
            }
            if (effective) {
                continue;
            }
            Finding finding{.kind = FindingKind::no_effect,
                            .file = std::string{file},
                            .line = entry.line,
                            .atom = std::string{text},
                            .token = token};
            if (!inside) {
                finding.message = "not in the IUSE of anything it matches";
            } else if (already) {
                finding.message = std::move(*already);
            } else if (overridden) {
                finding.kind = FindingKind::contradicted;
                finding.message = std::move(*overridden);
            } else {
                continue;
            }
            found.push_back(std::move(finding));
        }
    }
    return found;
}

std::vector<Finding> visibility_findings(const Store& installed, const RepositoryIndex& index) {
    std::set<std::string, std::less<>> installed_cps;
    for (const auto& package : installed.packages) {
        installed_cps.emplace(installed.string(package.cp));
    }
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp;
    for (std::uint32_t id = 0; id < index.versions.size(); ++id) {
        const auto cp = index.string(index.versions.at(id).cp);
        if (installed_cps.contains(cp)) {
            by_cp[std::string{cp}].push_back(id);
        }
    }
    const VersionMasks base{index};
    std::unordered_map<std::uint32_t, std::vector<std::string>> base_reasons;
    const auto reasons_of = [&](std::uint32_t id) -> const std::vector<std::string>& {
        auto found = base_reasons.find(id);
        if (found == base_reasons.end()) {
            found = base_reasons.emplace(id, base.reasons(id)).first;
        }
        return found->second;
    };
    std::vector<Finding> found;
    for (const auto& [entry_id, sets] : user_visibility_entries(index)) {
        const auto& entry = index.ledger_entries.at(entry_id);
        const auto text = index.string(entry.atom);
        const bool removal = text.starts_with('-');
        const auto atom = parse_config_atom(removal ? text.substr(1) : text);
        if (!atom) {
            continue;
        }
        std::vector<std::uint32_t> matched;
        const auto visit = [&](const std::string& cp, const std::vector<std::uint32_t>& ids) {
            for (const auto id : ids) {
                const auto& version = index.versions.at(id);
                const auto parsed = version_in(index.string(version.cpv), cp);
                if (parsed &&
                    matches(*atom, cp, *parsed, index.string(version.slot),
                            index.string(version.sub_slot),
                            index.string(index.repositories.at(version.repository).name))) {
                    matched.push_back(id);
                }
            }
        };
        if (atom->cp.contains('*')) {
            for (const auto& [cp, ids] : by_cp) {
                visit(cp, ids);
            }
        } else if (const auto hit = by_cp.find(atom->cp); hit != by_cp.end()) {
            visit(hit->first, hit->second);
        }
        if (matched.empty()) {
            continue;
        }
        // A mask line as a whole, else each token; a line without any (an accept_keywords line
        // accepting the ~ keywords) as a whole.
        std::vector<std::pair<LeftOut, std::string>> units;
        const auto tokens = index.ids_in(entry.tokens);
        if (sets == Sets::mask || sets == Sets::unmask || tokens.empty()) {
            units.emplace_back(LeftOut{.entry = entry_id}, std::string{});
        } else {
            std::uint32_t position = 0;
            for (const auto token : tokens) {
                units.emplace_back(LeftOut{.entry = entry_id, .position = position++},
                                   std::string{index.string(token)});
            }
        }
        for (auto& [left_out, token] : units) {
            const VersionMasks without{index, left_out};
            const bool effective = std::ranges::any_of(
                matched, [&](std::uint32_t id) { return reasons_of(id) != without.reasons(id); });
            if (effective) {
                continue;
            }
            const auto any_reason = [&](auto&& reasons) {
                return std::ranges::any_of(matched, [&](std::uint32_t id) {
                    return std::ranges::any_of(reasons(id), [&](const std::string& reason) {
                        return reason_of(sets, reason);
                    });
                });
            };
            const bool masked_without =
                any_reason([&](std::uint32_t id) { return without.reasons(id); });
            const auto file = index.string(entry.file);
            const bool refusing = token.starts_with('-');
            Finding finding{.kind = FindingKind::no_effect,
                            .file = std::string{file},
                            .line = entry.line,
                            .atom = std::string{text},
                            .token = std::move(token),
                            .message = "changes nothing for anything it matches"};
            // The first matched version's entry deciding it, as its place in parentheses.
            const auto placed = [&](auto&& decided_by) {
                for (const auto id : matched) {
                    if (const auto decider = decided_by(id)) {
                        return std::format(" ({})", entry_place(index, *decider, file));
                    }
                }
                return std::string{};
            };
            if (sets == Sets::mask && !removal) {
                if (!any_reason(reasons_of)) {
                    finding.kind = FindingKind::contradicted;
                    finding.message =
                        "unmasked again for everything it matches" +
                        placed([&](std::uint32_t id) { return base.unmasked_by(id); });
                } else {
                    finding.message =
                        "already masked" +
                        placed([&](std::uint32_t id) -> std::optional<std::uint32_t> {
                            for (const auto& reason : without.sourced_reasons(id)) {
                                if (reason.text == "package.mask" && !reason.entries.empty()) {
                                    return reason.entries.front();
                                }
                            }
                            return std::nullopt;
                        });
                }
            } else if (sets == Sets::unmask) {
                const auto unmasker =
                    placed([&](std::uint32_t id) { return without.unmasked_by(id); });
                finding.message = unmasker.empty() ? "nothing it matches is masked"
                                                   : "already unmasked" + unmasker;
            } else if (sets != Sets::mask && refusing && masked_without) {
                finding.message = "already refused";
            } else if (sets != Sets::mask && !refusing && !masked_without) {
                finding.message = "nothing it matches needs it";
            }
            found.push_back(std::move(finding));
        }
    }
    return found;
}

std::vector<Finding> check_config(const Store& installed, const Evaluated& evaluated,
                                  const RepositoryIndex& index) {
    const auto entries = user_entries(evaluated, index);
    auto found = unmatched_entries(entries, installed, index);
    for (auto more : {use_findings(installed, evaluated), visibility_findings(installed, index)}) {
        found.insert(found.end(), std::make_move_iterator(more.begin()),
                     std::make_move_iterator(more.end()));
    }
    order_findings(found);
    return found;
}

} // namespace egraph
