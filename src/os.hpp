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

// A process start() began. Destroying one that has not ended terminates it and reaps it.
class Child {
  public:
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept;
    Child& operator=(Child&& other) noexcept;
    ~Child();

    // How it ended, without waiting: its exit status, or why there is none; nothing while it
    // runs.
    [[nodiscard]] std::optional<std::expected<int, SpawnError>> poll();
    // How it ended, waiting for it.
    [[nodiscard]] std::expected<int, SpawnError> wait();

  private:
    friend std::expected<Child, SpawnError> start(const std::vector<std::string>& argv,
                                                  const std::optional<std::filesystem::path>& log);
    Child(int pid, std::string name);
    // Waits for the process, with or without blocking.
    std::optional<std::expected<int, SpawnError>> reap(bool block);
    void stop() noexcept;

    int pid_ = -1;
    std::string name_;
    std::optional<std::expected<int, SpawnError>> ended_;
};

// As run(), without waiting for the child.
std::expected<Child, SpawnError>
start(const std::vector<std::string>& argv,
      const std::optional<std::filesystem::path>& log = std::nullopt);

// Whether this process could create or replace a file at path by renaming a new one over it:
// the nearest existing directory above it is writable.
bool can_create(const std::filesystem::path& path);

// Replaces target with a copy of source so readers see the old file or the new one, never a
// mix: copied beside it (mode 0644), synced, renamed over it. Creates target's directories.
std::expected<void, std::error_code> replace_with_copy(const std::filesystem::path& source,
                                                       const std::filesystem::path& target);

// Whether this process runs as root (effective user id 0).
bool is_root();

// The running executable, from /proc/self/exe; empty if that cannot be read.
std::filesystem::path executable();

// Whether standard output is a terminal.
bool stdout_is_terminal();

// Whether standard input is a terminal.
bool stdin_is_terminal();

// Whether the locale the environment selects (LC_ALL, LC_CTYPE, LANG) has UTF-8 characters; not
// when it names a locale that is not installed.
bool utf8_locale();

// The environment variable's value, if it is set.
std::optional<std::string> environment(std::string_view name);

} // namespace egraph::os
