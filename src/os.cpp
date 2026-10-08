#include "os.hpp"

#include "build_info.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <langinfo.h>
#include <limits>
#include <malloc.h>
#include <poll.h>
#include <random>
#include <ranges>
#include <span>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#if EGRAPH_HAVE_JOURNAL
#include <systemd/sd-journal.h>
#endif

// posix_spawn hands the child our environment.
extern "C" {
extern char** environ; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}

namespace egraph::os {

namespace {

// posix_spawn file actions, destroyed with the object; none until to() is called.
class Redirections {
  public:
    Redirections() = default;
    Redirections(const Redirections&) = delete;
    Redirections& operator=(const Redirections&) = delete;
    Redirections(Redirections&&) = delete;
    Redirections& operator=(Redirections&&) = delete;
    ~Redirections() {
        if (used_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }

    // stdin from /dev/null, stdout and stderr to log; an errno value on failure.
    int to(const std::filesystem::path& log) {
        if (const int error = ::posix_spawn_file_actions_init(&actions_); error != 0) {
            return error;
        }
        used_ = true;
        log_ = log.string();
        if (const int error =
                ::posix_spawn_file_actions_addopen(&actions_, 0, "/dev/null", O_RDONLY, 0);
            error != 0) {
            return error;
        }
        if (const int error = ::posix_spawn_file_actions_addopen(
                &actions_, 1, log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            error != 0) {
            return error;
        }
        return ::posix_spawn_file_actions_adddup2(&actions_, 1, 2);
    }

    // stdin from input and stdout to output; an errno value on failure.
    int connect(int input, int output) {
        if (const int error = ::posix_spawn_file_actions_init(&actions_); error != 0) {
            return error;
        }
        used_ = true;
        if (const int error = ::posix_spawn_file_actions_adddup2(&actions_, input, 0); error != 0) {
            return error;
        }
        return ::posix_spawn_file_actions_adddup2(&actions_, output, 1);
    }

    [[nodiscard]] const posix_spawn_file_actions_t* get() const {
        return used_ ? &actions_ : nullptr;
    }

  private:
    posix_spawn_file_actions_t actions_{};
    // addopen keeps the pointer, not a copy, until the spawn.
    std::string log_;
    bool used_ = false;
};

// Starts argv, not empty, with redirections; its pid.
std::expected<int, SpawnError> spawn(const std::vector<std::string>& argv,
                                     const Redirections& redirections) {
    // posix_spawnp wants char* const[]: pointers into copies we own, then a terminator.
    std::vector<std::string> args = argv;
    std::vector<char*> pointers;
    pointers.reserve(args.size() + 1);
    for (auto& arg : args) {
        pointers.push_back(arg.data());
    }
    pointers.push_back(nullptr);

    pid_t pid = 0;
    const int error = ::posix_spawnp(&pid, args.front().c_str(), redirections.get(), nullptr,
                                     pointers.data(), environ);
    if (error != 0) {
        return std::unexpected(SpawnError{std::format(
            "{}: {}", args.front(), std::error_code(error, std::generic_category()).message())});
    }
    return pid;
}

SpawnError errno_error(std::string_view what) {
    return SpawnError{
        std::format("{}: {}", what, std::error_code(errno, std::generic_category()).message())};
}

std::uint64_t non_negative(std::int64_t value) {
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

// fsync(2) on path, a file or a directory.
std::error_code sync(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {errno, std::generic_category()};
    }
    const int error = ::fsync(fd) == 0 ? 0 : errno;
    ::close(fd);
    return error == 0 ? std::error_code{} : std::error_code{error, std::generic_category()};
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

std::expected<FileIdentity, std::error_code> identity(const std::filesystem::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return FileIdentity{
        .device = st.st_dev,
        .inode = st.st_ino,
        .mtime_ns = non_negative((static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000) +
                                 st.st_mtim.tv_nsec),
        .size = non_negative(st.st_size)};
}

std::expected<int, SpawnError> run(const std::vector<std::string>& argv,
                                   const std::optional<std::filesystem::path>& log) {
    return start(argv, log).and_then([](Child child) { return child.wait(); });
}

std::expected<Child, SpawnError> start(const std::vector<std::string>& argv,
                                       const std::optional<std::filesystem::path>& log) {
    if (argv.empty()) {
        return std::unexpected(SpawnError{"nothing to run"});
    }
    Redirections redirections;
    if (log) {
        if (const auto error = redirections.to(*log); error != 0) {
            return std::unexpected(
                SpawnError{std::format("{}: {}", log->string(),
                                       std::error_code(error, std::generic_category()).message())});
        }
    }
    return spawn(argv, redirections).transform([&argv](int pid) {
        return Child{pid, argv.front()};
    });
}

Descriptor::Descriptor(Descriptor&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

Descriptor& Descriptor::operator=(Descriptor&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Descriptor::~Descriptor() {
    close();
}

void Descriptor::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Talk::Talk(Child child, Descriptor input, Descriptor output)
    : child_{std::move(child)}, input_{std::move(input)}, output_{std::move(output)} {}

bool Talk::send(std::string_view line) {
    if (input_.get() < 0) {
        return false;
    }
    const std::string text = std::string{line} + '\n';
    for (std::span<const char> rest{text}; !rest.empty();) {
        const auto sent = ::send(input_.get(), rest.data(), rest.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        rest = rest.subspan(static_cast<std::size_t>(sent));
    }
    return true;
}

std::optional<std::string> Talk::receive() {
    std::array<char, 4096> buffer{};
    for (;;) {
        if (const auto end = pending_.find('\n'); end != std::string::npos) {
            auto line = pending_.substr(0, end);
            pending_.erase(0, end + 1);
            return line;
        }
        const auto got =
            output_.get() < 0 ? 0 : ::read(output_.get(), buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            output_.close();
            if (pending_.empty()) {
                return std::nullopt;
            }
            return std::exchange(pending_, {});
        }
        pending_.append_range(std::span{buffer}.first(static_cast<std::size_t>(got)));
    }
}

bool Talk::ready() const {
    return output_.get() < 0 || pending_.contains('\n');
}

void Talk::fill() {
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto got = ::read(output_.get(), buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            output_.close();
            return;
        }
        pending_.append_range(std::span{buffer}.first(static_cast<std::size_t>(got)));
        return;
    }
}

std::expected<Jobserver, std::error_code> Jobserver::open(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return Jobserver{Descriptor{fd}};
}

std::expected<std::optional<std::byte>, std::error_code> Jobserver::take() {
    std::array<std::byte, 1> token{};
    for (;;) {
        const auto got = ::read(fd_.get(), token.data(), token.size());
        if (got == 1) {
            return token.front();
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return std::nullopt;
        }
        return std::unexpected(got == 0 ? std::make_error_code(std::errc::broken_pipe)
                                        : std::error_code(errno, std::generic_category()));
    }
}

std::expected<void, std::error_code> Jobserver::give(std::byte token) {
    const std::array<std::byte, 1> given{token};
    for (;;) {
        const auto put = ::write(fd_.get(), given.data(), given.size());
        if (put == 1) {
            return {};
        }
        if (put < 0 && errno == EINTR) {
            continue;
        }
        return std::unexpected(put == 0 ? std::make_error_code(std::errc::broken_pipe)
                                        : std::error_code(errno, std::generic_category()));
    }
}

std::expected<Readiness, std::error_code>
wait_for(std::span<Talk> talks,
         const std::optional<std::reference_wrapper<const Jobserver>>& jobserver,
         std::optional<std::chrono::milliseconds> timeout) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = timeout ? std::optional{Clock::now() + *timeout} : std::nullopt;
    const auto ready = [&] {
        std::vector<std::size_t> found;
        for (const auto& [i, talk] : std::views::enumerate(talks)) {
            if (talk.ready()) {
                found.push_back(static_cast<std::size_t>(i));
            }
        }
        return found;
    };
    for (;;) {
        if (auto found = ready(); !found.empty()) {
            return Readiness{.talks = std::move(found), .token = false};
        }
        std::vector<pollfd> watched;
        watched.reserve(talks.size() + 1);
        for (const auto& talk : talks) {
            watched.push_back({.fd = talk.output_.get(), .events = POLLIN, .revents = 0});
        }
        if (jobserver) {
            watched.push_back({.fd = jobserver->get().fd_.get(), .events = POLLIN, .revents = 0});
        }
        if (watched.empty()) {
            return Readiness{};
        }
        int wait = -1;
        if (deadline) {
            const auto left =
                std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now());
            wait = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(
                left.count(), 0, std::numeric_limits<int>::max()));
        }
        const auto events = ::poll(watched.data(), watched.size(), wait);
        if (events < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        if (events == 0) {
            return Readiness{.talks = {}, .token = false, .timed_out = true};
        }
        for (const auto& [talk, polled] : std::views::zip(talks, watched)) {
            if (polled.revents != 0) {
                talk.fill();
            }
        }
        if (jobserver && watched.back().revents != 0) {
            return Readiness{.talks = ready(), .token = true};
        }
    }
}

std::expected<int, SpawnError> Talk::finish() {
    input_.close();
    return child_.wait();
}

std::expected<Talk, SpawnError> start_talking(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::unexpected(SpawnError{"nothing to run"});
    }
    std::array<int, 2> input{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input.data()) != 0) {
        return std::unexpected(errno_error("socketpair"));
    }
    Descriptor ours_in{input.front()};
    Descriptor theirs_in{input.back()};
    std::array<int, 2> output{-1, -1};
    if (::pipe2(output.data(), O_CLOEXEC) != 0) {
        return std::unexpected(errno_error("pipe2"));
    }
    Descriptor ours_out{output.front()};
    Descriptor theirs_out{output.back()};
    // Only the child reads what we send.
    ::shutdown(ours_in.get(), SHUT_RD);
    Redirections redirections;
    if (const auto error = redirections.connect(theirs_in.get(), theirs_out.get()); error != 0) {
        return std::unexpected(SpawnError{std::format(
            "{}: {}", argv.front(), std::error_code(error, std::generic_category()).message())});
    }
    const auto pid = spawn(argv, redirections);
    if (!pid) {
        return std::unexpected(pid.error());
    }
    return Talk{Child{*pid, argv.front()}, std::move(ours_in), std::move(ours_out)};
}

Child::Child(int pid, std::string name) : pid_{pid}, name_{std::move(name)} {}

Child::Child(Child&& other) noexcept
    : pid_{std::exchange(other.pid_, -1)}, name_{std::move(other.name_)},
      ended_{std::move(other.ended_)} {}

Child& Child::operator=(Child&& other) noexcept {
    if (this != &other) {
        stop();
        pid_ = std::exchange(other.pid_, -1);
        name_ = std::move(other.name_);
        ended_ = std::move(other.ended_);
    }
    return *this;
}

Child::~Child() {
    stop();
}

void Child::stop() noexcept {
    if (pid_ < 0 || ended_) {
        return;
    }
    ::kill(pid_, SIGTERM);
    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
}

std::optional<std::expected<int, SpawnError>> Child::poll() {
    return ended_ ? ended_ : reap(false);
}

std::expected<int, SpawnError> Child::wait() {
    if (!ended_) {
        (void)reap(true);
    }
    return ended_.value_or(std::unexpected(SpawnError{"no process"}));
}

std::optional<std::expected<int, SpawnError>> Child::reap(bool block) {
    if (pid_ < 0) {
        return std::nullopt;
    }
    int status = 0;
    pid_t reaped = 0;
    while ((reaped = ::waitpid(pid_, &status, block ? 0 : WNOHANG)) < 0) {
        if (errno != EINTR) {
            ended_ = std::unexpected(SpawnError{std::format(
                "waitpid: {}", std::error_code(errno, std::generic_category()).message())});
            return ended_;
        }
    }
    if (reaped == 0) {
        return std::nullopt;
    }
    if (WIFEXITED(status)) {
        ended_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ended_ = std::unexpected(
            SpawnError{std::format("{}: killed by signal {}", name_, WTERMSIG(status))});
    } else {
        ended_ = std::unexpected(SpawnError{std::format("{}: stopped", name_)});
    }
    return ended_;
}

namespace {

// What the stop handler touches, the only things a signal handler may: a flag, and a pipe that
// wakes a Watcher's poll(2) whenever the signal comes. Globals, as a handler reaches nothing else.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t stop_asked = 0;
int stop_read = -1;
int stop_write = -1;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void on_stop(int /*signal*/) {
    stop_asked = 1;
    const char byte = 0;
    // A full pipe has a byte waiting already.
    [[maybe_unused]] const auto written = ::write(stop_write, &byte, 1);
}

std::error_code last_error() {
    return {errno, std::generic_category()};
}

} // namespace

void release_memory() {
    ::malloc_trim(0);
}

std::expected<void, std::error_code> catch_stop_signals() {
    if (stop_write >= 0) {
        return {};
    }
    std::array<int, 2> ends{-1, -1};
    if (::pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(last_error());
    }
    stop_read = ends.at(0);
    stop_write = ends.at(1);
    struct sigaction action{};
    action.sa_handler = on_stop;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        if (::sigaction(signal, &action, nullptr) != 0) {
            return std::unexpected(last_error());
        }
    }
    return {};
}

std::expected<Watcher, std::error_code> Watcher::open() {
    const int fd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    return Watcher{Descriptor{fd}};
}

namespace {

// Written files count once closed, not at every write.
constexpr std::uint32_t watched_events = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                         IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF |
                                         IN_MOVE_SELF | IN_ONLYDIR;

// The watch descriptor of each event in bytes read from inotify, -1 for an overflow.
std::vector<int> event_watches(std::span<const std::byte> bytes) {
    constexpr std::size_t header = sizeof(int) + (3 * sizeof(std::uint32_t));
    const auto field = [&](std::size_t at) {
        std::array<std::byte, 4> word{};
        std::ranges::copy(bytes.subspan(at, word.size()), word.begin());
        return word;
    };
    std::vector<int> found;
    for (std::size_t at = 0; at + header <= bytes.size();) {
        found.push_back(std::bit_cast<int>(field(at)));
        at += header + std::bit_cast<std::uint32_t>(field(at + header - sizeof(std::uint32_t)));
    }
    return found;
}

} // namespace

std::expected<void, std::error_code> Watcher::add(const std::filesystem::path& directory) {
    const int watch = ::inotify_add_watch(fd_.get(), directory.c_str(), watched_events);
    if (watch < 0) {
        return std::unexpected(last_error());
    }
    counted_.push_back(watch);
    return {};
}

std::expected<void, std::error_code> Watcher::add_aside(const std::filesystem::path& directory) {
    const int watch = ::inotify_add_watch(fd_.get(), directory.c_str(), watched_events);
    if (watch < 0) {
        return std::unexpected(last_error());
    }
    aside_.push_back(watch);
    return {};
}

std::expected<Woken, std::error_code>
Watcher::wait(std::optional<std::chrono::milliseconds> timeout, Pollable also) {
    if (stop_asked != 0) {
        return Woken{.changed = false, .stop = true, .ready = false};
    }
    if (!std::in_range<short>(also.events)) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    std::array<pollfd, 3> watched{
        {{.fd = fd_.get(), .events = POLLIN, .revents = 0},
         {.fd = stop_read, .events = POLLIN, .revents = 0},
         {.fd = also.fd, .events = static_cast<short>(also.events), .revents = 0}}};
    int wait = -1;
    if (timeout) {
        wait = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(
            timeout->count(), 0, std::numeric_limits<int>::max()));
    }
    // poll(2) skips a negative descriptor, as the pipe is before catch_stop_signals().
    const auto events = ::poll(watched.data(), watched.size(), wait);
    if (events < 0 && errno != EINTR) {
        return std::unexpected(last_error());
    }
    Woken woken{.changed = false,
                .stop = stop_asked != 0,
                .ready = events > 0 && watched.at(2).revents != 0};
    if (events > 0 && watched.at(0).revents != 0) {
        // What changed matters less than that something did: the refresh finds out.
        std::array<std::byte, 16384> events_read{};
        for (ssize_t got = 0;
             (got = ::read(fd_.get(), events_read.data(), events_read.size())) > 0;) {
            for (const auto watch :
                 event_watches(std::span{events_read}.first(static_cast<std::size_t>(got)))) {
                const bool aside =
                    std::ranges::contains(aside_, watch) && !std::ranges::contains(counted_, watch);
                woken.changed = woken.changed || !aside;
                woken.aside = woken.aside || aside;
            }
        }
    }
    return woken;
}

bool can_create(const std::filesystem::path& path) {
    std::error_code error;
    auto dir = path.parent_path();
    while (!dir.empty() && !std::filesystem::exists(dir, error) && dir != dir.parent_path()) {
        dir = dir.parent_path();
    }
    return !dir.empty() && access(dir.c_str(), W_OK) == 0;
}

std::expected<std::uint64_t, std::error_code> free_bytes(const std::filesystem::path& path) {
    auto existing = path;
    std::error_code ignored;
    while (!std::filesystem::exists(existing, ignored) && existing.has_parent_path() &&
           existing.parent_path() != existing) {
        existing = existing.parent_path();
    }
    struct statvfs found{};
    if (::statvfs(existing.c_str(), &found) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return std::uint64_t{found.f_bsize} * std::uint64_t{found.f_bavail};
}

std::int64_t process_id() {
    return ::getpid();
}

bool is_root() {
    return geteuid() == 0;
}

std::filesystem::path executable() {
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
}

std::expected<FileIdentity, std::error_code> running_identity() {
    return identity("/proc/self/exe");
}

std::expected<std::vector<std::string>, std::error_code> command_line() {
    std::ifstream in{"/proc/self/cmdline", std::ios::binary};
    if (!in) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    std::vector<std::string> found;
    for (std::string arg; std::getline(in, arg, '\0');) {
        found.push_back(std::move(arg));
    }
    if (found.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    return found;
}

std::error_code exec(const std::filesystem::path& file, const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    // execv wants char* const[], as posix_spawnp does.
    std::vector<std::string> args = argv;
    std::vector<char*> pointers;
    pointers.reserve(args.size() + 1);
    for (auto& arg : args) {
        pointers.push_back(arg.data());
    }
    pointers.push_back(nullptr);
    ::execv(file.c_str(), pointers.data());
    return {errno, std::generic_category()};
}

bool stdout_is_terminal() {
    return isatty(STDOUT_FILENO) == 1;
}

bool stdin_is_terminal() {
    return isatty(STDIN_FILENO) == 1;
}

std::expected<void, std::error_code> append_locked(const std::filesystem::path& path,
                                                   std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected(error);
        }
    }
    if (!std::filesystem::exists(path, error)) {
        // Made here rather than by open's O_CREAT, whose mode is a vararg.
        std::ofstream{path, std::ios::app}.close();
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            error);
        if (error) {
            return std::unexpected(error);
        }
    }
    Descriptor fd{::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC)};
    if (fd.get() < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    // From offset 0, where a new descriptor starts, to the end: the whole file, the POSIX record
    // lock Python's fcntl.lockf takes for portage.
    while (::lockf(fd.get(), F_LOCK, 0) != 0) {
        if (errno != EINTR) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
    }
    while (!text.empty()) {
        const auto put = ::write(fd.get(), text.data(), text.size());
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        text.remove_prefix(static_cast<std::size_t>(put));
    }
    // Closing lets the lock go.
    return {};
}

bool journal_built() {
    return EGRAPH_HAVE_JOURNAL != 0;
}

std::expected<void, std::error_code> journal_send(std::span<const std::string> fields) {
#if EGRAPH_HAVE_JOURNAL
    // sd_journal_sendv takes mutable buffers.
    std::vector<std::string> copies(fields.begin(), fields.end());
    std::vector<iovec> vectors;
    vectors.reserve(copies.size());
    for (auto& field : copies) {
        vectors.push_back({.iov_base = field.data(), .iov_len = field.size()});
    }
    if (vectors.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(std::make_error_code(std::errc::argument_list_too_long));
    }
    const int sent = sd_journal_sendv(vectors.data(), static_cast<int>(vectors.size()));
    if (sent < 0) {
        return std::unexpected(std::error_code(-sent, std::generic_category()));
    }
    return {};
#else
    std::ignore = fields;
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
#endif
}

namespace {

// Where a replacement for target is written before it is renamed over target.
std::filesystem::path replacement_of(const std::filesystem::path& target) {
    std::random_device random;
    return target.parent_path() / std::format(".{}.{:08x}", target.filename().string(), random());
}

// Renames the replacement written at temp over target, readable by all and synced first.
std::expected<void, std::error_code> rename_over(const std::filesystem::path& temp,
                                                 const std::filesystem::path& target) {
    namespace fs = std::filesystem;
    const auto discard = [&temp](std::error_code why) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return std::unexpected(why);
    };
    std::error_code error;
    fs::permissions(temp,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read,
                    error);
    if (error) {
        return discard(error);
    }
    if (const auto unsynced = sync(temp)) {
        return discard(unsynced);
    }
    fs::rename(temp, target, error);
    if (error) {
        return discard(error);
    }
    if (const auto unsynced = sync(target.parent_path())) {
        return std::unexpected(unsynced);
    }
    return {};
}

} // namespace

std::expected<void, std::error_code> replace_with_copy(const std::filesystem::path& source,
                                                       const std::filesystem::path& target) {
    namespace fs = std::filesystem;
    const auto absolute = fs::absolute(target);
    std::error_code error;
    fs::create_directories(absolute.parent_path(), error);
    if (error) {
        return std::unexpected(error);
    }
    const auto temp = replacement_of(absolute);
    // copy_file refuses to overwrite, so the name is ours.
    fs::copy_file(source, temp, error);
    if (error) {
        return std::unexpected(error);
    }
    return rename_over(temp, absolute);
}

std::expected<void, std::error_code> replace_with_text(const std::filesystem::path& target,
                                                       std::string_view text) {
    namespace fs = std::filesystem;
    const auto absolute = fs::absolute(target);
    std::error_code error;
    fs::create_directories(absolute.parent_path(), error);
    if (error) {
        return std::unexpected(error);
    }
    const auto temp = replacement_of(absolute);
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        out << text;
        out.close();
        if (!out) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
    }
    return rename_over(temp, absolute);
}

std::expected<void, std::error_code> append_replacing(const std::filesystem::path& path,
                                                      std::string_view text) {
    namespace fs = std::filesystem;
    const auto absolute = fs::absolute(path);
    std::error_code error;
    fs::create_directories(absolute.parent_path(), error);
    if (error) {
        return std::unexpected(error);
    }
    Descriptor directory{
        ::open(absolute.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.get() < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    while (::flock(directory.get(), LOCK_EX) != 0) {
        if (errno != EINTR) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
    }
    std::string whole;
    if (fs::exists(absolute, error)) {
        std::ifstream in{absolute, std::ios::binary};
        std::ostringstream read;
        read << in.rdbuf();
        if (!in) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        whole = std::move(read).str();
    }
    whole += text;
    // Closing the directory lets the lock go.
    return replace_with_text(absolute, whole);
}

bool utf8_locale() {
    // A locale of our own, so the process's stays "C" for everything else.
    locale_t locale = newlocale(LC_CTYPE_MASK, "", locale_t{});
    if (locale == locale_t{}) {
        return false;
    }
    const std::string_view codeset{nl_langinfo_l(CODESET, locale)};
    const bool utf8 = codeset == "UTF-8";
    freelocale(locale);
    return utf8;
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
