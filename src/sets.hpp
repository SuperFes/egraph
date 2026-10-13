#pragma once

// Package sets beyond those the store holds, as egraph-build lists them from portage's set
// configuration.

#include "query.hpp"
#include "request.hpp"
#include "store.hpp"

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

// "@name<TAB>class<TAB>world<TAB>description" for each set the list holds, by name; "world" is
// empty unless emerge records the set in world_sets.
[[nodiscard]] std::vector<std::string> set_lines(const SetList& list);

// For each set named, in order, each of its atoms: "@name<TAB>@holder<TAB>atom<TAB>cpvs", the
// holder the set the atom is in (the one named, or one nested in it), cpvs the installed
// packages the atom matches, space-separated.
[[nodiscard]] std::vector<std::string>
set_atom_lines(const Store& store, std::span<const std::string> names, const Sets& sets);

} // namespace egraph
