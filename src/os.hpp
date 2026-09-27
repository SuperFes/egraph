#pragma once

// The only place egraph calls C APIs directly; everything here returns values.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace egraph::os {

enum class FileKind : std::uint8_t { file, directory, symlink };

struct FileStatus {
    FileKind kind = FileKind::file;
    // Clamped at 0, as the builder records it.
    std::uint64_t mtime_ns = 0;
    std::uint64_t size = 0;
};

// lstat(2): a symlink is reported as itself, not its target.
std::expected<FileStatus, std::error_code> lstat(const std::filesystem::path& path);

struct SpawnError {
    std::string message;
};

// Runs argv (argv[0] looked up in PATH) with our stdio and environment, and waits for it.
std::expected<int, SpawnError> run(const std::vector<std::string>& argv);

} // namespace egraph::os
