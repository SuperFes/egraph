#include "verify.hpp"

#include "human.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace egraph {

std::vector<std::string> pretend_arguments(const EmergeRequest& request) {
    // Not --usepkg=n: emerge keeps it as an option given, which turns off --with-bdeps' default
    // (upstream-notes.md).
    std::vector<std::string> arguments{"--pretend", "--verbose", "--color=n", "--nospinner",
                                       "--ignore-default-opts"};
    if (request.update) {
        arguments.emplace_back("--update");
    }
    if (request.deep) {
        arguments.emplace_back("--deep");
    }
    if (request.noreplace) {
        arguments.emplace_back("--noreplace");
    }
    if (request.rebuilds == UseRebuilds::all) {
        arguments.emplace_back("--newuse");
    } else if (request.rebuilds == UseRebuilds::changed) {
        arguments.emplace_back("--changed-use");
    }
    if (!request.dynamic_deps) {
        arguments.emplace_back("--dynamic-deps=n");
    }
    arguments.insert(arguments.end(), request.targets.begin(), request.targets.end());
    return arguments;
}

namespace {

bool use_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The NAME="..." groups in what follows a merge's cpv, joined by spaces.
std::string use_groups(std::string_view text) {
    std::string groups;
    for (auto equals = text.find("=\""); equals != std::string_view::npos;
         equals = text.find("=\"", equals + 1)) {
        auto start = equals;
        while (start > 0 && use_name_char(text.at(start - 1))) {
            --start;
        }
        if (start == equals || (start > 0 && text.at(start - 1) != ' ')) {
            continue;
        }
        const auto close = text.find('"', equals + 2);
        if (close == std::string_view::npos) {
            break;
        }
        groups +=
            std::format("{}{}", groups.empty() ? "" : " ", text.substr(start, close + 1 - start));
        equals = close;
    }
    return groups;
}

// The package an uninstall line shows, "[uninstall     ] cpv:slot::repo".
std::optional<PretendMerge> parse_uninstall(std::string_view line) {
    constexpr std::string_view type = "[uninstall ";
    const auto close = line.find("] ");
    if (!line.starts_with(type) || close == std::string_view::npos) {
        return std::nullopt;
    }
    auto package = line.substr(close + 2);
    package = package.substr(0, std::min(package.find(' '), package.size()));
    const auto separator = package.find("::");
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    return PretendMerge{.cpv =
                            std::string{package.substr(0, std::min(package.find(':'), separator))},
                        .repo = std::string{package.substr(separator + 2)},
                        .kind = "uninstall",
                        .use = ""};
}

// The blocker an unresolved blocker's line shows, once per holder:
// "[blocks B      ] atom ("atom" is soft blocking cpv, cpv)", or "(is hard blocking cpv)".
std::vector<PretendBlock> parse_block(std::string_view line) {
    constexpr std::string_view type = "[blocks B";
    const auto close = line.find("] ");
    if (!line.starts_with(type) || close == std::string_view::npos) {
        return {};
    }
    auto rest = line.substr(close + 2);
    const auto open = rest.find(" (");
    if (open == std::string_view::npos || !rest.ends_with(')')) {
        return {};
    }
    auto atom = rest.substr(0, open);
    auto note = rest.substr(open + 2, rest.size() - open - 3);
    if (note.starts_with('"')) {
        const auto quote = note.find('"', 1);
        if (quote == std::string_view::npos) {
            return {};
        }
        atom = note.substr(1, quote - 1);
        note.remove_prefix(quote + 2);
    }
    const auto blocking = note.find("blocking ");
    if (!note.starts_with("is ") || blocking == std::string_view::npos) {
        return {};
    }
    std::vector<PretendBlock> blocks;
    for (auto holders = note.substr(blocking + 9); !holders.empty();) {
        const auto comma = std::min(holders.find(", "), holders.size());
        blocks.push_back(
            {.atom = std::string{atom}, .holder = std::string{holders.substr(0, comma)}});
        holders.remove_prefix(std::min(comma + 2, holders.size()));
    }
    return blocks;
}

// The merge a list line shows, "[ebuild  NS    ] cpv:slot::repo [old] USE=... size".
std::optional<PretendMerge> parse_merge(std::string_view line) {
    std::string_view rest;
    for (const std::string_view type : {"[ebuild ", "[binary "}) {
        if (line.starts_with(type)) {
            rest = line.substr(type.size());
        }
    }
    const auto close = rest.find("] ");
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    // Columns: interactive, N (or r), S (or R), fetch, U, D, then the mask.
    const auto status = rest.substr(0, close);
    const auto column = [status](std::size_t index) {
        return index < status.size() ? status.at(index) : ' ';
    };
    std::string kind = column(1) == 'N'   ? (column(2) == 'S' ? "new-slot" : "new")
                       : column(4) != 'U' ? "rebuild"
                       : column(5) == 'D' ? "downgrade"
                                          : "upgrade";
    rest = rest.substr(close + 2);
    const auto end = std::min(rest.find(' '), rest.size());
    const auto package = rest.substr(0, end);
    const auto separator = package.find("::");
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto cpv = package.substr(0, std::min(package.find(':'), separator));
    return PretendMerge{.cpv = std::string{cpv},
                        .repo = std::string{package.substr(separator + 2)},
                        .kind = std::move(kind),
                        .use = use_groups(rest.substr(end))};
}

std::string_view kind_text(UpdateKind kind) {
    switch (kind) {
    case UpdateKind::upgrade:
        return "upgrade";
    case UpdateKind::downgrade:
        return "downgrade";
    case UpdateKind::rebuild:
        break;
    }
    return "rebuild";
}

// Whether a and b are the same cp in the same repository at versions that compare equal.
bool equal_versions(const PretendMerge& a, const PretendMerge& b) {
    const auto left = split_cpv(a.cpv);
    const auto right = split_cpv(b.cpv);
    if (a.repo != b.repo || left.category != right.category || left.name != right.name) {
        return false;
    }
    const auto one = parse_version(left.version);
    const auto other = parse_version(right.version);
    return one && other && vercmp(*one, *other) == 0;
}

// The atom of "emerge: there are no ebuilds to satisfy "atom"." or of "!!! All ebuilds that
// could satisfy "atom" have been masked.".
std::optional<std::string> parse_unsatisfied(std::string_view line) {
    for (const auto& [before, after] :
         {std::pair<std::string_view, std::string_view>{
              "emerge: there are no ebuilds to satisfy \"", "\"."},
          {"!!! All ebuilds that could satisfy \"", "\" have been masked."}}) {
        if (line.starts_with(before) && line.ends_with(after) &&
            line.size() > before.size() + after.size()) {
            return std::string{
                line.substr(before.size(), line.size() - before.size() - after.size())};
        }
    }
    return std::nullopt;
}

// "cpv flags", the flags by name, of a package.use line ">=cpv flags", ">=cpv:slot flags" or
// "=cpv flags".
std::optional<std::string> parse_use_change(std::string_view line) {
    const auto space = line.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    auto atom = line.substr(0, space);
    atom.remove_prefix(atom.starts_with(">=") ? 2 : atom.starts_with('=') ? 1 : 0);
    atom = atom.substr(0, std::min(atom.find(':'), atom.size()));
    std::vector<std::string_view> flags;
    for (auto rest = line.substr(space + 1); !rest.empty();) {
        const auto end = std::min(rest.find(' '), rest.size());
        if (end > 0) {
            flags.push_back(rest.substr(0, end));
        }
        rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    // emerge lists them in a set's order.
    std::ranges::sort(flags, [](std::string_view a, std::string_view b) {
        return a.substr(a.starts_with('-') ? 1 : 0) < b.substr(b.starts_with('-') ? 1 : 0);
    });
    std::string change{atom};
    for (const auto flag : flags) {
        change += std::format(" {}", flag);
    }
    return change;
}

template <typename T> void sort_unique(std::vector<T>& values) {
    std::ranges::sort(values);
    const auto [first, last] = std::ranges::unique(values);
    values.erase(first, last);
}

} // namespace

Pretend parse_pretend(std::string_view output, bool failed) {
    Pretend found;
    // The line after an unmet requirements message: "- cpv::repo USE=...".
    bool unmet_next = false;
    // Inside the USE changes block, up to the blank line ending it.
    bool in_use_changes = false;
    while (!output.empty()) {
        const auto newline = std::min(output.find('\n'), output.size());
        const auto line = output.substr(0, newline);
        const bool unmet_line = std::exchange(unmet_next, false);
        if (in_use_changes) {
            in_use_changes = !line.empty();
            if (in_use_changes && !line.starts_with('#') && !line.starts_with(' ')) {
                if (auto change = parse_use_change(line)) {
                    found.use_changes.push_back(std::move(*change));
                }
            }
        } else if (line == "The following USE changes are necessary to proceed:") {
            in_use_changes = true;
        } else if (unmet_line && failed && line.starts_with("- ")) {
            const auto key = line.substr(2);
            found.unmet.emplace_back(key.substr(0, std::min(key.find(' '), key.size())));
        } else if (line.starts_with("!!! The ebuild selected to satisfy \"") &&
                   line.ends_with("\" has unmet requirements.")) {
            unmet_next = true;
        } else if (auto merge = parse_merge(line)) {
            found.merges.push_back(std::move(*merge));
        } else if (auto uninstall = parse_uninstall(line)) {
            found.merges.push_back(std::move(*uninstall));
        } else if (auto atom = parse_unsatisfied(line); atom && failed) {
            found.unsatisfied.push_back(std::move(*atom));
        } else {
            std::ranges::move(parse_block(line), std::back_inserter(found.blocks));
        }
        output.remove_prefix(std::min(newline + 1, output.size()));
    }
    sort_unique(found.blocks);
    sort_unique(found.unsatisfied);
    sort_unique(found.unmet);
    sort_unique(found.use_changes);
    return found;
}

Pretend planned_merges(const Store& store, const Evaluated& original, const Plan& plan) {
    const auto& evaluated = plan.evaluated_or(original);
    Pretend found;
    found.merges.reserve(plan.merges.size() + plan.uninstalls.size());
    for (const auto& merge : plan.merges) {
        const auto& candidate = evaluated.candidates.at(merge.candidate);
        found.merges.push_back(PretendMerge{
            .cpv = std::string{evaluated.string(candidate.cpv)},
            .repo = std::string{evaluated.string(candidate.repo)},
            .kind = std::string{merge.replaces ? kind_text(merge.kind)
                                : other_slots(store, evaluated, merge).empty() ? "new"
                                                                               : "new-slot"},
            .use = merge.replaces ? std::string{} : use_display(evaluated, candidate)});
    }
    for (const auto& each : plan.uninstalls) {
        // emerge leaves a replaced slot to its depclean.
        if (!each.why) {
            continue;
        }
        const auto& pkg = store.packages.at(each.package);
        found.merges.push_back(PretendMerge{.cpv = std::string{store.string(pkg.cpv)},
                                            .repo = std::string{store.string(pkg.repo)},
                                            .kind = "uninstall",
                                            .use = ""});
    }
    for (const auto& block : plan.blocks) {
        const auto& holder = block.holder;
        const auto atom = std::string_view{block.atom};
        found.blocks.push_back(
            {.atom = std::string{atom.substr(std::min(atom.find_first_not_of('!'), atom.size()))},
             .holder = std::string{holder.candidate
                                       ? evaluated.string(evaluated.candidates.at(holder.index).cpv)
                                       : store.string(store.packages.at(holder.index).cpv)}});
    }
    for (const auto& each : plan.unsatisfied) {
        found.unsatisfied.push_back(each.atom);
    }
    for (const auto index : plan.unmet) {
        const auto& candidate = evaluated.candidates.at(index);
        found.unmet.push_back(std::format("{}::{}", evaluated.string(candidate.cpv),
                                          evaluated.string(candidate.repo)));
    }
    for (const auto& needed : plan.use_changes) {
        if (auto change = parse_use_change(package_use_line(store, evaluated, needed.change))) {
            found.use_changes.push_back(std::move(*change));
        }
    }
    sort_unique(found.blocks);
    sort_unique(found.unsatisfied);
    sort_unique(found.unmet);
    sort_unique(found.use_changes);
    return found;
}

std::vector<std::string> merge_differences(const Pretend& our_list, const Pretend& their_list) {
    if (!their_list.use_changes.empty()) {
        // Both sorted.
        std::vector<std::string> lines;
        for (const auto& [from, against, side] :
             {std::tuple{&their_list.use_changes, &our_list.use_changes,
                         std::string_view{"emerge"}},
              std::tuple{&our_list.use_changes, &their_list.use_changes,
                         std::string_view{"egraph"}}}) {
            std::vector<std::string> only;
            std::ranges::set_difference(*from, *against, std::back_inserter(only));
            for (const auto& change : only) {
                const auto space = std::min(change.find(' '), change.size());
                lines.push_back(std::format("{}\t{}\tuse-change {}", change.substr(0, space), side,
                                            change.substr(std::min(space + 1, change.size()))));
            }
        }
        std::ranges::sort(lines);
        return lines;
    }
    if (!their_list.unsatisfied.empty() || !their_list.unmet.empty()) {
        // Both sorted.
        std::vector<std::string> lines;
        for (const auto& [theirs, ours, kind] :
             {std::tuple{&their_list.unsatisfied, &our_list.unsatisfied,
                         std::string_view{"unsatisfied"}},
              std::tuple{&their_list.unmet, &our_list.unmet, std::string_view{"required-use"}}}) {
            std::vector<std::string> missed;
            std::ranges::set_difference(*theirs, *ours, std::back_inserter(missed));
            for (const auto& key : missed) {
                lines.push_back(std::format("{}\temerge\t{}", key, kind));
            }
        }
        std::ranges::sort(lines);
        return lines;
    }
    const auto& ours = our_list.merges;
    const auto& theirs = their_list.merges;
    // Our merges emerge's have not matched yet, as indices into ours.
    std::map<std::pair<std::string, std::string>, std::size_t> left;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        left.emplace(std::pair{ours.at(i).cpv, ours.at(i).repo}, i);
    }
    std::vector<std::string> lines;
    const auto compare = [&lines](const PretendMerge& our, const PretendMerge& their) {
        const auto key = std::format("{}::{}", our.cpv, our.repo);
        if (our.kind != their.kind) {
            lines.push_back(std::format("{}\tkind\t{}\t{}", key, our.kind, their.kind));
        } else if ((our.kind == "new" || our.kind == "new-slot") && our.use != their.use) {
            lines.push_back(std::format("{}\tuse\t{}\t{}", key, our.use, their.use));
        }
    };
    std::vector<std::size_t> unmatched;
    for (std::size_t i = 0; i < theirs.size(); ++i) {
        const auto& their = theirs.at(i);
        const auto found = left.find(std::pair{their.cpv, their.repo});
        if (found == left.end()) {
            unmatched.push_back(i);
            continue;
        }
        compare(ours.at(found->second), their);
        left.erase(found);
    }
    // emerge picks among equal versions in directory order (upstream-notes.md).
    for (const auto i : unmatched) {
        const auto& their = theirs.at(i);
        const auto found = std::ranges::find_if(
            left, [&](const auto& entry) { return equal_versions(ours.at(entry.second), their); });
        if (found != left.end()) {
            compare(ours.at(found->second), their);
            left.erase(found);
        } else {
            lines.push_back(std::format("{}::{}\temerge\t{}", their.cpv, their.repo, their.kind));
        }
    }
    for (const auto& [key, index] : left) {
        lines.push_back(
            std::format("{}::{}\tegraph\t{}", key.first, key.second, ours.at(index).kind));
    }
    // Both sorted.
    std::vector<PretendBlock> only;
    std::ranges::set_difference(our_list.blocks, their_list.blocks, std::back_inserter(only));
    for (const auto& block : only) {
        lines.push_back(std::format("{}\tegraph\tblocks {}", block.holder, block.atom));
    }
    only.clear();
    std::ranges::set_difference(their_list.blocks, our_list.blocks, std::back_inserter(only));
    for (const auto& block : only) {
        lines.push_back(std::format("{}\temerge\tblocks {}", block.holder, block.atom));
    }
    for (const auto& atom : our_list.unsatisfied) {
        lines.push_back(std::format("{}\tegraph\tunsatisfied", atom));
    }
    for (const auto& key : our_list.unmet) {
        lines.push_back(std::format("{}\tegraph\trequired-use", key));
    }
    std::ranges::sort(lines);
    return lines;
}

} // namespace egraph
