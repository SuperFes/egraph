#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace egraph::snapshot {

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
