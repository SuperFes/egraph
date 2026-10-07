#pragma once

// The status file: the updates egraph watch plans after a refresh, summed up for what opens
// without running anything (a status bar, the interface).

#include "history.hpp"
#include "plan.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

inline constexpr int status_format = 1;

// The plan the status holds, as egraph's arguments.
inline constexpr std::string_view status_command = "updates --world -D -N --held";

// A store file and the build start it records.
struct StoreBuild {
    std::string path;
    std::uint64_t built = 0;
    auto operator<=>(const StoreBuild&) const = default;
};

// The stores a status was planned from, each where it was read: the repository index may be the
// system's beside a user's installed store.
struct StatusStores {
    StoreBuild installed;
    StoreBuild evaluated;
    StoreBuild repository;
    auto operator<=>(const StatusStores&) const = default;
};

struct PlanCounts {
    std::size_t upgrades = 0;
    std::size_t downgrades = 0;
    // The same version merged again: for USE, a slot operator, or a masked installed version.
    std::size_t rebuilds = 0;
    // New in their slot.
    std::size_t added = 0;
    std::size_t held = 0;
    std::size_t uninstalls = 0;
    std::size_t masked = 0;
    // emerge would refuse the plan.
    bool refused = false;
    auto operator<=>(const PlanCounts&) const = default;
};

struct RepositorySync {
    std::string name;
    // When its snapshot was made, from metadata/timestamp.chk as emerge --info shows it; none
    // without one (a plain git checkout).
    std::optional<Seconds> synced{};
    auto operator<=>(const RepositorySync&) const = default;
};

struct Status {
    Seconds written{};
    StatusStores stores;
    PlanCounts counts;
    std::vector<RepositorySync> repositories;
    // status_command's output as lines.
    std::vector<std::string> lines;
    auto operator<=>(const Status&) const = default;
};

[[nodiscard]] PlanCounts plan_counts(const Plan& plan);

// timestamp.chk's first line: "Tue, 06 Oct 2026 18:46:01 +0000".
[[nodiscard]] std::optional<Seconds> parse_sync_timestamp(std::string_view text);
[[nodiscard]] std::optional<Seconds> repository_synced(const std::filesystem::path& location);

// As one JSON object and a newline; parse_status takes it back, refusing another format.
// With current, also whether it is current against its stores.
[[nodiscard]] std::string status_json(const Status& status,
                                      std::optional<bool> current = std::nullopt);
[[nodiscard]] std::expected<Status, std::string> parse_status(std::string_view text);

// Beside the installed store: status.json.
[[nodiscard]] std::filesystem::path status_path(const std::filesystem::path& installed);

// Whether each store file still records the build start it did: whether a status planned from
// them is current.
[[nodiscard]] bool stores_unchanged(const StatusStores& stores);

// "just now", "1 minute ago", "5 hours ago", "3 days ago": from then to now, rounded down.
[[nodiscard]] std::string age_text(Seconds then, Seconds now);

// For a terminal: the counts that are not zero ("143 upgrades, 4 rebuilds, 2 held", or "no
// updates"), and that emerge would refuse the plan; then each repository's sync age; then when it
// was planned, or that the stores changed since (not current).
[[nodiscard]] std::vector<std::string> status_summary(const Status& status, bool current,
                                                      Seconds now);

// For a status bar, "key<TAB>value": each count, refused and current as yes or no, written in
// seconds, then "synced<TAB>repository<TAB>seconds" for each repository, empty without a time.
[[nodiscard]] std::vector<std::string> status_lines(const Status& status, bool current);

// Whether a refresh that left the stores at now plans anew, recorded being the stores the
// status file was planned from (none without a readable one); with sync, only when the
// repository index differs.
[[nodiscard]] bool status_due(PlanWhen when, const std::optional<StatusStores>& recorded,
                              const StatusStores& now);

} // namespace egraph
