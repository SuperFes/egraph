#include "config_check.hpp"

#include <algorithm>
#include <format>
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

std::vector<Finding> check_config(const Store& /*installed*/, const Evaluated& /*evaluated*/,
                                  const RepositoryIndex& /*index*/) {
    return {};
}

} // namespace egraph
