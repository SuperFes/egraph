#include "os.hpp"

#include <cerrno>
#include <cstdlib>
#include <format>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// posix_spawn hands the child our environment.
extern "C" {
extern char** environ; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}

namespace egraph::os {

namespace {

std::uint64_t non_negative(std::int64_t value) {
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

} // namespace

std::expected<FileStatus, std::error_code> lstat(const std::filesystem::path& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    FileStatus status;
    if (S_ISLNK(st.st_mode)) {
        status.kind = FileKind::symlink;
    } else if (S_ISDIR(st.st_mode)) {
        status.kind = FileKind::directory;
    }
    status.mtime_ns = non_negative((static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000) +
                                   st.st_mtim.tv_nsec);
    status.size = non_negative(st.st_size);
    return status;
}

std::expected<int, SpawnError> run(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::unexpected(SpawnError{"nothing to run"});
    }
    // posix_spawnp wants char* const[]: pointers into copies we own, then a terminator.
    std::vector<std::string> args = argv;
    std::vector<char*> pointers;
    pointers.reserve(args.size() + 1);
    for (auto& arg : args) {
        pointers.push_back(arg.data());
    }
    pointers.push_back(nullptr);

    pid_t pid = 0;
    const int error =
        ::posix_spawnp(&pid, args.front().c_str(), nullptr, nullptr, pointers.data(), environ);
    if (error != 0) {
        return std::unexpected(SpawnError{std::format(
            "{}: {}", args.front(), std::error_code(error, std::generic_category()).message())});
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected(SpawnError{std::format(
                "waitpid: {}", std::error_code(errno, std::generic_category()).message())});
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return std::unexpected(
            SpawnError{std::format("{}: killed by signal {}", args.front(), WTERMSIG(status))});
    }
    return std::unexpected(SpawnError{std::format("{}: stopped", args.front())});
}

bool can_create(const std::filesystem::path& path) {
    std::error_code error;
    auto dir = path.parent_path();
    while (!dir.empty() && !std::filesystem::exists(dir, error) && dir != dir.parent_path()) {
        dir = dir.parent_path();
    }
    return !dir.empty() && access(dir.c_str(), W_OK) == 0;
}

std::filesystem::path executable() {
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
}

bool stdout_is_terminal() {
    return isatty(STDOUT_FILENO) == 1;
}

std::optional<std::string> environment(std::string_view name) {
    // getenv needs a terminated string.
    const std::string key{name};
    const char* value =
        std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe): read once, single-threaded
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
}

} // namespace egraph::os
