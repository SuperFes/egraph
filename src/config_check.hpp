#pragma once

// egraph config check: the entries of the user's configuration (the files under its
// /etc/portage) that do nothing, or that later entries undo.

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// By severity: dead and contradicted are errors, no_effect a warning, not_installed a note.
enum class FindingKind : std::uint8_t {
    // Matches nothing in any repository.
    dead,
    // Later entries undo it for everything it matches.
    contradicted,
    // Changes nothing for anything it matches.
    no_effect,
    // Matches only packages not installed.
    not_installed,
};

struct Finding {
    FindingKind kind = FindingKind::dead;
    std::string file;
    std::uint32_t line = 0;
    std::string atom;
    // The token concerned; empty where the whole entry is.
    std::string token{};
    std::string message{};
};

[[nodiscard]] std::string_view kind_name(FindingKind kind);
[[nodiscard]] std::string_view severity_name(FindingKind kind);

// By kind, then file, then line; a line's findings keep their order.
void order_findings(std::vector<Finding>& findings);

// Whether any is an error.
[[nodiscard]] bool failing(std::span<const Finding> findings);

// "file<TAB>line<TAB>severity<TAB>kind<TAB>atom<TAB>token<TAB>message".
[[nodiscard]] std::string finding_record(const Finding& finding);

// The findings over the user's entries in both ledgers, ordered.
[[nodiscard]] std::vector<Finding> check_config(const Store& installed, const Evaluated& evaluated,
                                                const RepositoryIndex& index);

} // namespace egraph
