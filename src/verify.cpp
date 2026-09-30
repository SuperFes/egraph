#include "verify.hpp"

#include "human.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
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
    std::string kind = column(1) == 'N'   ? "new"
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

} // namespace

std::vector<PretendMerge> parse_pretend(std::string_view output) {
    std::vector<PretendMerge> merges;
    while (!output.empty()) {
        const auto newline = std::min(output.find('\n'), output.size());
        if (auto merge = parse_merge(output.substr(0, newline))) {
            merges.push_back(std::move(*merge));
        }
        output.remove_prefix(std::min(newline + 1, output.size()));
    }
    return merges;
}

std::vector<PretendMerge> planned_merges(const Evaluated& evaluated, const Plan& plan) {
    std::vector<PretendMerge> merges;
    merges.reserve(plan.merges.size());
    for (const auto& merge : plan.merges) {
        const auto& candidate = evaluated.candidates.at(merge.candidate);
        merges.push_back(PretendMerge{
            .cpv = std::string{evaluated.string(candidate.cpv)},
            .repo = std::string{evaluated.string(candidate.repo)},
            .kind = std::string{merge.replaces ? kind_text(merge.kind) : "new"},
            .use = merge.replaces ? std::string{} : use_display(evaluated, candidate)});
    }
    return merges;
}

std::vector<std::string> merge_differences(const std::vector<PretendMerge>& ours,
                                           const std::vector<PretendMerge>& theirs) {
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
        } else if (our.kind == "new" && our.use != their.use) {
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
    std::ranges::sort(lines);
    return lines;
}

} // namespace egraph
