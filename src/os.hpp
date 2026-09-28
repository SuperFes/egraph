#pragma once

// The only place egraph calls C APIs directly; everything here returns values.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

// Runs argv (argv[0] looked up in PATH) with our environment, and waits for it. With a log, the
// child's stdout and stderr go to that file (replaced) and its stdin is /dev/null; otherwise it
// shares our stdio.
std::expected<int, SpawnError> run(const std::vector<std::string>& argv,
                                   const std::optional<std::filesystem::path>& log = std::nullopt);

// Whether this process could create or replace a file at path by renaming a new one over it:
// the nearest existing directory above it is writable.
bool can_create(const std::filesystem::path& path);

// The running executable, from /proc/self/exe; empty if that cannot be read.
std::filesystem::path executable();

// Whether standard output is a terminal.
bool stdout_is_terminal();

// The environment variable's value, if it is set.
std::optional<std::string> environment(std::string_view name);

} // namespace egraph::os
