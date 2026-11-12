#pragma once

#include "history.hpp"
#include "os.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace egraph {
struct RunSettings;
}

namespace egraph::snapshot {

// What decides whether a run can go in a snapshot of its root.
struct Facts {
    // The root is a btrfs subvolume.
    bool snapshottable = false;
    bool root = false;
    // On the root's filesystem: what is free, and what the snapshot and images may take.
    std::uint64_t free = 0;
    std::uint64_t needed = 0;
};

// The run goes on the root itself; with why, when auto could not snapshot.
struct Plain {
    std::optional<std::string> why;
    bool operator==(const Plain&) const = default;
};
struct InSnapshot {
    bool operator==(const InSnapshot&) const = default;
};
// on asked for a snapshot that cannot be taken.
struct Refused {
    std::string why;
    bool operator==(const Refused&) const = default;
};
using Decision = std::variant<Plain, InSnapshot, Refused>;

// How a run goes, as exec's --snapshot or --no-snapshot (flag) or else the setting says.
[[nodiscard]] Decision decide(SnapshotRun setting, std::optional<bool> flag, const Facts& facts);

// The setting as the flag overrides it.
[[nodiscard]] SnapshotRun asked(SnapshotRun setting, std::optional<bool> flag);

// What a run in a snapshot may take on the root's filesystem: the merges' files, written in the
// snapshot and again as binary packages, by each merge's installed size where known.
[[nodiscard]] std::uint64_t needed_bytes(std::span<const std::optional<std::uint64_t>> sizes);

// binds normalized and sorted so each directory comes before those inside it, a directory given
// twice writable if either is.
[[nodiscard]] std::vector<os::Bind> ordered(std::vector<os::Bind> binds);

// What a worker in a snapshot shares with the real root, ordered: the kernel's filesystems and
// /run, where builds run and what they fetch or cache; the repositories read-only.
[[nodiscard]] std::vector<os::Bind> binds(const RunSettings& settings);

// The hidden egraph enter command line that runs argv confined to the snapshot at root.
[[nodiscard]] std::vector<std::string> enter_command(const std::filesystem::path& egraph,
                                                     const std::filesystem::path& root,
                                                     std::span<const os::Bind> binds,
                                                     std::span<const std::string> argv);

// The commands of the btrfs backend, as argv.
std::vector<std::string> show_command(const std::filesystem::path& path);
std::vector<std::string> create_command(const std::filesystem::path& source,
                                        const std::filesystem::path& destination);
std::vector<std::string> remove_command(const std::filesystem::path& path);

// Whether path is the root of a btrfs subvolume, which is what a snapshot can be taken of.
bool snapshottable(const std::filesystem::path& path);

// A read-write snapshot of the subvolume source at destination, which must not exist. Subvolumes
// nested in source are left out, as btrfs does.
std::expected<void, std::string> create(const std::filesystem::path& source,
                                        const std::filesystem::path& destination);

// Deletes the snapshot at destination.
std::expected<void, std::string> remove(const std::filesystem::path& destination);

} // namespace egraph::snapshot
