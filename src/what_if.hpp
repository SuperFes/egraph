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

// existing, the text of the file what_if_path gives for lines' kind, with lines saved: in
// egraph's own file (own) one line per atom, each token replacing those naming the same flag
// (USE_EXPAND prefixes written out) or env file, a line left without tokens dropped, comments
// kept; in the user's single file, the lines appended as written.
[[nodiscard]] std::string saved_text(std::string_view existing, std::span<const WhatIfLine> lines,
                                     bool own);

// A file saving lines writes, and its whole new text.
struct SavedFile {
    std::filesystem::path path;
    std::string text;
};

// The files saving lines writes under user_config, package.use's first, as saved_text makes
// them from what they hold now; why one could not be read.
[[nodiscard]] std::expected<std::vector<SavedFile>, std::string>
saved_files(const std::filesystem::path& user_config, std::span<const WhatIfLine> lines);

// Writes each file through a file beside it renamed over it, keeping its permissions, its
// directory made where missing; why one could not be written.
[[nodiscard]] std::expected<void, std::string> write_saved(std::span<const SavedFile> files);

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

// An installed package whose own version's ebuild gets other env files with lines tried.
struct EnvChange {
    // Installed package id.
    std::uint32_t package = 0;
    std::vector<std::string> before;
    std::vector<std::string> after;
};

// The installed packages lines tried build with other env files, by id: those package.env gives
// their own version's ebuild without and with them (tried, what with_what_if made of untried),
// and for every package the files of a */* line tried.
[[nodiscard]] std::vector<EnvChange> env_changes(const Store& installed, const Evaluated& untried,
                                                 const Evaluated& tried,
                                                 std::span<const WhatIfLine> lines);

// "cpv<TAB>env<TAB>before<TAB>after" per change, each list of files space-separated.
[[nodiscard]] std::vector<std::string> env_lines(const Store& installed,
                                                 std::span<const EnvChange> changes);

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
