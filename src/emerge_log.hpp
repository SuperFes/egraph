#pragma once

// emerge's builds as emerge.log records them, for the build history: read only, never written,
// and coarser than a worker's records (whole seconds, no phases, nothing measured), each marked
// by its origin.

#include "builds.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// The origin of a record read from emerge.log.
inline constexpr std::string_view emerge_log_origin = "emerge.log";

// A build already in the history is one of the same cpv starting this close to it.
inline constexpr double import_tolerance = 60;

// Where emerge keeps its log: ${EPREFIX}/var/log/emerge.log, whatever the root.
[[nodiscard]] std::filesystem::path emerge_log_path(const std::filesystem::path& eprefix);

// The builds the log's text records compiling and merging into eroot (emerge names the EROOT),
// in the order they started:
// each from its ">>> emerge" line to its "=== Merging" line, its merge from there to its
// "::: completed emerge"; its jobs from its session's options (the "*** emerge" line, which holds
// EMERGE_DEFAULT_OPTS' too): --jobs=N, none for --jobs alone, else one. Binary packages merged,
// builds that failed and builds cut short are left out.
[[nodiscard]] std::vector<BuildRecord> emerge_log_builds(std::string_view text,
                                                         std::string_view eroot);

struct Imported {
    std::string text{};
    std::size_t added = 0;
};

// The history's text with records added, but those it holds already (import_tolerance); every
// record in order of start, each cp pruned to its last keep, and the lines that do not parse
// kept first, as they are.
[[nodiscard]] Imported imported_builds(std::string_view history,
                                       std::span<const BuildRecord> records, std::size_t keep);

} // namespace egraph
