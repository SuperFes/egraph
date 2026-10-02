#pragma once

// The only place egraph calls C APIs directly; everything here returns values.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
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

class Talk;
class Jobserver;
struct Readiness;

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
    friend std::expected<Talk, SpawnError> start_talking(const std::vector<std::string>& argv);
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

// A file descriptor this process owns, closed with the object.
class Descriptor {
  public:
    Descriptor() = default;
    explicit Descriptor(int fd) : fd_{fd} {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept;
    Descriptor& operator=(Descriptor&& other) noexcept;
    ~Descriptor();

    [[nodiscard]] int get() const { return fd_; }
    void close() noexcept;

  private:
    int fd_ = -1;
};

// A process start_talking() began, its standard input and output connected to this process, its
// standard error ours. Destroying one that has not ended terminates it, as a Child.
class Talk {
  public:
    // Whether receive() returns without waiting: a line has come, or the output has ended.
    [[nodiscard]] bool ready() const;
    // Writes line and a newline to its standard input; false once it reads no more.
    [[nodiscard]] bool send(std::string_view line);
    // The next line it writes, without the newline; none once its output ends.
    [[nodiscard]] std::optional<std::string> receive();
    // Closes its standard input and waits for it to end.
    [[nodiscard]] std::expected<int, SpawnError> finish();

  private:
    friend std::expected<Talk, SpawnError> start_talking(const std::vector<std::string>& argv);
    friend std::expected<Readiness, std::error_code>
    wait_for(std::span<Talk> talks,
             const std::optional<std::reference_wrapper<const Jobserver>>& jobserver);
    Talk(Child child, Descriptor input, Descriptor output);
    // Reads what has come, once, without waiting if poll(2) said something has.
    void fill();

    Child child_;
    Descriptor input_;
    Descriptor output_;
    // Read beyond the last line received.
    std::string pending_;
};

// Runs argv (argv[0] looked up in PATH) with our environment, without waiting for it, to talk to
// a line at a time. Its standard input is a socket, so that writing to one that has ended fails
// rather than raising SIGPIPE.
std::expected<Talk, SpawnError> start_talking(const std::vector<std::string>& argv);

// A make jobserver's named pipe, as emerge's FEATURES=jobserver-token uses one: a byte read from
// it is a token to run one job, written back when the job ends.
class Jobserver {
  public:
    // Opens the pipe at path for reading and writing without blocking, as emerge opens it.
    [[nodiscard]] static std::expected<Jobserver, std::error_code>
    open(const std::filesystem::path& path);
    // A token, if one is free now.
    [[nodiscard]] std::expected<std::optional<std::byte>, std::error_code> take();
    [[nodiscard]] std::expected<void, std::error_code> give(std::byte token);

  private:
    friend std::expected<Readiness, std::error_code>
    wait_for(std::span<Talk> talks,
             const std::optional<std::reference_wrapper<const Jobserver>>& jobserver);
    explicit Jobserver(Descriptor fd) : fd_{std::move(fd)} {}

    Descriptor fd_;
};

// What wait_for() found.
struct Readiness {
    // Indices of the talks that are ready().
    std::vector<std::size_t> talks;
    // The jobserver may have a token.
    bool token = false;
};

// Waits until a talk is ready() or, given one, the jobserver may have a token; nothing found
// when there is nothing to wait for.
std::expected<Readiness, std::error_code>
wait_for(std::span<Talk> talks,
         const std::optional<std::reference_wrapper<const Jobserver>>& jobserver = std::nullopt);

// Whether this process could create or replace a file at path by renaming a new one over it:
// the nearest existing directory above it is writable.
bool can_create(const std::filesystem::path& path);

// Replaces target with a copy of source so readers see the old file or the new one, never a
// mix: copied beside it (mode 0644), synced, renamed over it. Creates target's directories.
std::expected<void, std::error_code> replace_with_copy(const std::filesystem::path& source,
                                                       const std::filesystem::path& target);

// The bytes free to this process on the filesystem of path, or of the nearest directory above it
// that exists.
std::expected<std::uint64_t, std::error_code> free_bytes(const std::filesystem::path& path);

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
