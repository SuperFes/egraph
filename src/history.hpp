#pragma once

// The system store's history: the generations a refresh replaced, in ${EPREFIX}/var/lib/egraph.

#include "store.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// After which refreshes egraph watch plans the updates for the status file: each, those that
// rebuilt the repository index (a sync or a configuration edit), or none.
enum class PlanWhen : std::uint8_t { refresh, sync, never };

// egraph's settings, from ${PORTAGE_CONFIGROOT}/etc/egraph/egraph.conf.
struct Settings {
    // How many days of generations to keep; 0 keeps none.
    int history_days = 90;
    PlanWhen plan = PlanWhen::refresh;
    // After how many days since its sync a repository is a notice; 0 never.
    int stale_days = 7;
    // The terminal egraph notify's Open starts the interface in, before its command, as words;
    // empty, one found.
    std::string terminal{};
};

// "key = value" lines, blank lines and "#" comments skipped. An error names the line of an
// unknown key or a value that does not parse.
[[nodiscard]] std::expected<Settings, std::string> parse_settings(std::string_view text);
[[nodiscard]] std::filesystem::path settings_path(const std::filesystem::path& config_root);
// The settings at path; the defaults when there is none.
[[nodiscard]] std::expected<Settings, std::string> read_settings(const std::filesystem::path& path);

// ${ROOT}${EPREFIX}/var/lib/egraph
[[nodiscard]] std::filesystem::path history_directory(const std::filesystem::path& root,
                                                      const std::filesystem::path& eprefix);

using Seconds = std::chrono::sys_seconds;

// A generation's file name from the time the system stopped being as its store holds it (the
// replacing store's build time), in UTC: installed-20261006T162600Z.egraph; and back, none for
// any other name.
[[nodiscard]] std::string generation_name(Seconds ended);
[[nodiscard]] std::optional<Seconds> generation_time(std::string_view name);

// Of the generations built at these times, those to delete at now: kept are all from the last
// day, then the newest of each UTC day up to days old.
[[nodiscard]] std::vector<Seconds> thinned(std::span<const Seconds> generations, Seconds now,
                                           int days);

// One change between two stores, a line of the history log.
struct HistoryEvent {
    // When it merged (from the package's merge time), or for an uninstall, when it was found.
    Seconds time;
    // merged, upgraded, downgraded (replacing another version in its slot), rebuilt (the same
    // version merged again), uninstalled.
    std::string event;
    std::string cpv;
    // The version upgraded or downgraded from; empty for the others.
    std::string from{};
};

// What changed from before to after, found at now: oldest first, merges before uninstalls,
// then by cpv.
[[nodiscard]] std::vector<HistoryEvent> history_events(const Store& before, const Store& after,
                                                       Seconds now);
// The event as a line of JSON, without the newline.
[[nodiscard]] std::string event_line(const HistoryEvent& event);
// The log's events, skipping lines that are not one.
[[nodiscard]] std::vector<HistoryEvent> parse_history(std::string_view text);

// Which events history shows: those since a time, of the packages named.
struct HistoryQuery {
    std::optional<Seconds> since{};
    // Exact cpvs or portage atoms, matched by cp and version: the log has no slots, repositories
    // or USE.
    std::vector<std::string> packages{};
};

// history's arguments: an age or a date (as diff takes them), at most one, and packages.
[[nodiscard]] std::expected<HistoryQuery, std::string>
parse_history_query(std::span<const std::string> arguments, Seconds now,
                    const std::chrono::time_zone& zone);

// The events the query selects, in the log's order; an error for a package argument that is not
// an atom history can match.
[[nodiscard]] std::expected<std::vector<HistoryEvent>, std::string>
selected_events(std::span<const HistoryEvent> events, const HistoryQuery& query);

// Each cp's first event that brought a version in (any but an uninstall), in the events' order.
[[nodiscard]] std::vector<HistoryEvent> arrivals(std::span<const HistoryEvent> events);

// For the human layout: the time in zone, then the event as a difference_lines record.
[[nodiscard]] std::vector<std::string> event_records(std::span<const HistoryEvent> events,
                                                     const std::chrono::time_zone& zone);

// Whether after differs from before in its installed packages or its root sets.
[[nodiscard]] bool history_changed(const Store& before, const Store& after);

// What diff compares against: the system as it was at a time, or a generation by its name; neither
// for the newest generation.
struct DiffBase {
    std::optional<Seconds> at{};
    std::optional<std::string> generation{};
};

// diff's argument: an age (12h, 3d, 2w) or a date (2026-09-30, its start in zone), or a
// generation's file name.
[[nodiscard]] std::expected<DiffBase, std::string>
parse_diff_base(std::string_view text, Seconds now, const std::chrono::time_zone& zone);

// Of the generations, by when each ended, the one holding the system as it was at `at`: the
// oldest that ended after it, none when none did (it is as it was then); without `at`, the newest.
[[nodiscard]] std::optional<Seconds> generation_at(std::span<const Seconds> generations,
                                                   std::optional<Seconds> at);

// A change from one store to another: an installed package's, or an atom of a root set's.
struct Difference {
    // upgrade, downgrade, rebuild (merged again, or its USE changed), new, uninstall; added or
    // removed for an atom.
    std::string kind;
    // The cpvs, before empty for new and after for uninstall; for an atom, the set and the atom.
    std::string before{};
    std::string after{};
    // Flags turned on ("+flag") and off ("-flag"), by flag.
    std::vector<std::string> use{};
};

// Upgrades, downgrades, rebuilds, new and uninstalled packages, each by cpv, then the root sets'
// atoms by set.
[[nodiscard]] std::vector<Difference> differences(const Store& before, const Store& after);

// As tab-separated lines: before, kind, after and the flags (separated by spaces) for a package;
// the set, kind and atom for an atom.
[[nodiscard]] std::vector<std::string> difference_lines(std::span<const Difference> changes);

// {"changes":[...],"generation":name,"since":seconds}, the generation omitted without one.
void write_differences_json(std::ostream& out, std::span<const Difference> changes, Seconds since,
                            std::optional<std::string_view> generation);

} // namespace egraph
