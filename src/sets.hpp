#pragma once

// Package sets beyond those the store holds, as egraph-build lists them from portage's set
// configuration.

#include "query.hpp"
#include "request.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

struct PackageSet {
    std::string name;
    // The class portage builds it with; empty for a name it does not define.
    std::string kind;
    std::string description;
    // Whether emerge records it in world_sets when asked for it without --oneshot.
    bool world_candidate = false;
    // Asked for by name: its atoms as emerge's arguments hold them, a nested set's under its own
    // name.
    std::optional<std::vector<Argument>> atoms;
    // Why emerge could not load it.
    std::optional<std::string> error;
};

struct SetList {
    std::vector<PackageSet> sets;
    // What portage's set configuration could not read.
    std::vector<std::string> errors;
};

// egraph-build --sets' output.
[[nodiscard]] std::expected<SetList, std::string> parse_sets(std::string_view text);

// Each set the list holds the atoms of, by name; the first that could not be loaded instead,
// as "@name: why".
[[nodiscard]] std::expected<Sets, std::string> loaded_sets(const SetList& list);

} // namespace egraph
