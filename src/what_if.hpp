#pragma once

#include "evaluated.hpp"
#include "store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// A configuration line tried before it is saved, as it would be written to egraph's own file.
struct WhatIfLine {
    enum class File : std::uint8_t { use, env };
    File file = File::use;
    // A configuration atom, or "*/*" for every package.
    std::string atom;
    // As written: flags (USE_EXPAND's "VAR:" prefixes included) or env file names.
    std::vector<std::string> tokens;

    bool operator==(const WhatIfLine&) const = default;
};

// text as a line of file: "atom tokens...", or for package.use, flags alone for every package.
[[nodiscard]] std::expected<WhatIfLine, std::string> parse_what_if(WhatIfLine::File file,
                                                                   std::string_view text);

// Where file's lines are saved under the user's configuration directory: package.use/egraph
// (package.env/egraph), or package.use itself where that is a file.
[[nodiscard]] std::filesystem::path what_if_path(const std::filesystem::path& user_config,
                                                 WhatIfLine::File file);

// evaluated as if lines were saved to what_if_path: each line an entry of the USE ledger where
// that file is read among the user's (a "*/*" line of package.use's among the conf layer's, as
// portage folds it there; of package.env's, last), each candidate whose USE that changes
// restacked and its dependencies reduced under its new USE (with_use_changes). The error for a
// line naming an env file the ledger has not read.
[[nodiscard]] std::expected<Evaluated, std::string>
with_what_if(Evaluated evaluated, const Store& installed, std::span<const WhatIfLine> lines,
             const std::filesystem::path& user_config);

// The cps that dependencies of the candidates whose USE tried changes (blockers aside) name,
// which have ebuilds in the repositories but were never evaluated; tried is what with_what_if
// made of before.
[[nodiscard]] std::vector<std::string> newly_reached(const Evaluated& before,
                                                     const Evaluated& tried);

// What lines tried change in a plan, from update_lines' records (without a table) of the plan
// made without them (before) and with them (after): per merge, by its first field (the installed
// cpv it replaces, or a new package's cpv),
// "first<TAB>tried<TAB>change<TAB>kind<TAB>target cpv<TAB>repo<TAB>flags", change added (only
// after), dropped (only before, with before's fields) or changed (another kind, target, repo or
// flags: a rebuild's flags, a new package's USE). The added and changed in after's order, then the
// dropped in before's.
[[nodiscard]] std::vector<std::string> tried_lines(std::span<const std::string> before,
                                                   std::span<const std::string> after);

} // namespace egraph
