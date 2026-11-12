#include "snapshot.hpp"

#include "action.hpp"
#include "os.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace egraph::snapshot {

namespace {

// Runs argv quietly: why not, with the last line it wrote, if it failed.
std::expected<void, std::string> succeeded(const std::vector<std::string>& argv) {
    const auto log = std::filesystem::temp_directory_path() /
                     std::format("egraph-snapshot-{}.log", os::process_id());
    const auto status = os::run(argv, log);
    std::string last;
    if (std::ifstream in{log}; in) {
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) {
                last = std::move(line);
            }
        }
    }
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    if (!status) {
        return std::unexpected(status.error().message);
    }
    if (*status != 0) {
        return std::unexpected(
            last.empty() ? std::format("{} failed with status {}", argv.front(), *status) : last);
    }
    return {};
}

} // namespace

SnapshotRun asked(SnapshotRun setting, std::optional<bool> flag) {
    if (flag) {
        return *flag ? SnapshotRun::on : SnapshotRun::off;
    }
    return setting;
}

Decision decide(SnapshotRun setting, std::optional<bool> flag, const Facts& facts) {
    const auto mode = asked(setting, flag);
    if (mode == SnapshotRun::off) {
        return Plain{};
    }
    std::optional<std::string> why;
    if (!facts.root) {
        why = "only root or the subvolume's owner can take a snapshot of it";
    } else if (!facts.snapshottable) {
        why = "the root is not a btrfs subvolume";
    } else if (facts.needed > facts.free) {
        why = std::format("the snapshot may take {} bytes, and {} are free", facts.needed,
                          facts.free);
    }
    if (!why) {
        return InSnapshot{};
    }
    if (mode == SnapshotRun::on) {
        return Refused{.why = std::move(*why)};
    }
    return Plain{.why = std::move(why)};
}

std::uint64_t needed_bytes(std::span<const std::optional<std::uint64_t>> sizes) {
    constexpr auto most = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total = 0;
    for (const auto& size : sizes) {
        const auto bytes = size.value_or(0);
        total = bytes > most - total ? most : total + bytes;
    }
    return total > most / 2 ? most : total * 2;
}

std::vector<os::Bind> ordered(std::vector<os::Bind> binds) {
    for (auto& bind : binds) {
        bind.path = bind.path.lexically_normal();
        if (bind.path.has_parent_path() && !bind.path.has_filename()) {
            bind.path = bind.path.parent_path();
        }
    }
    // Writable first among equal paths, so the one kept is.
    std::ranges::sort(binds, [](const os::Bind& a, const os::Bind& b) {
        return a.path != b.path ? a.path < b.path : !a.read_only && b.read_only;
    });
    const auto [first, last] = std::ranges::unique(binds, {}, &os::Bind::path);
    binds.erase(first, last);
    return binds;
}

std::vector<os::Bind> binds(const RunSettings& settings) {
    std::vector<os::Bind> found;
    for (const std::string_view path : {"/dev", "/proc", "/sys", "/run"}) {
        found.push_back({.path = path, .read_only = false});
    }
    found.push_back({.path = settings.tmpdir, .read_only = false});
    found.push_back({.path = settings.distdir, .read_only = false});
    if (settings.ccache_dir) {
        found.push_back({.path = *settings.ccache_dir, .read_only = false});
    }
    if (settings.logdir) {
        found.push_back({.path = *settings.logdir, .read_only = false});
    }
    if (settings.elog_summary) {
        found.push_back({.path = std::filesystem::path{*settings.elog_summary}.parent_path(),
                         .read_only = false});
    }
    for (const auto& repository : settings.repositories) {
        found.push_back({.path = repository, .read_only = true});
    }
    return ordered(std::move(found));
}

RunPaths run_paths(const std::filesystem::path& eroot, std::int64_t pid) {
    const auto state = eroot / "var/lib/egraph";
    return {.snapshot = state / std::format("snapshot-{}", pid), .packages = state / "packages"};
}

std::vector<std::filesystem::path> other_volumes(std::span<const std::filesystem::path> paths) {
    const auto root = os::identity("/");
    std::vector<std::filesystem::path> found;
    for (const auto& path : paths) {
        const auto identity = os::identity(path);
        if (root && identity && identity->device != root->device) {
            found.push_back(path);
        }
    }
    return found;
}

std::vector<os::Bind> worker_binds(const RunSettings& settings,
                                   const std::filesystem::path& packages,
                                   std::span<const std::filesystem::path> foreign) {
    auto found = binds(settings);
    found.push_back({.path = packages, .read_only = false});
    for (const auto& path : foreign) {
        found.push_back({.path = path, .read_only = true});
    }
    return ordered(std::move(found));
}

std::vector<std::string> enter_command(const std::filesystem::path& egraph,
                                       const std::filesystem::path& snapshot,
                                       const std::filesystem::path& target,
                                       std::span<const os::Bind> binds,
                                       std::span<const std::string> argv) {
    std::vector<std::string> command{egraph.string(), "enter"};
    for (const auto& bind : binds) {
        command.insert(command.end(),
                       {bind.read_only ? "--read-only" : "--bind", bind.path.string()});
    }
    command.insert(command.end(), {"--target", target.string(), snapshot.string(), "--"});
    command.insert(command.end(), argv.begin(), argv.end());
    return command;
}

std::vector<std::string> create_command(const std::filesystem::path& source,
                                        const std::filesystem::path& destination) {
    return {"btrfs", "subvolume", "snapshot", source.string(), destination.string()};
}

std::vector<std::string> remove_command(const std::filesystem::path& path) {
    return {"btrfs", "subvolume", "delete", path.string()};
}

bool snapshottable(const std::filesystem::path& path) {
    return os::btrfs_subvolume(path);
}

std::expected<void, std::string> create(const std::filesystem::path& source,
                                        const std::filesystem::path& destination) {
    return succeeded(create_command(source, destination));
}

std::expected<void, std::string> remove(const std::filesystem::path& destination) {
    const auto deleted = succeeded(remove_command(destination));
    if (deleted) {
        return {};
    }
    // Its owner, not root, may still remove it once emptied, as rmdir lets them.
    std::error_code error;
    std::filesystem::remove_all(destination, error);
    if (error) {
        return std::unexpected(std::format("{}; {}", deleted.error(), error.message()));
    }
    return {};
}

} // namespace egraph::snapshot
