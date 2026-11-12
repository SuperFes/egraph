#include "snapshot.hpp"

#include "action.hpp"
#include "os.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <string_view>
#include <utility>

namespace egraph::snapshot {

namespace {

std::expected<void, std::string> succeeded(const std::vector<std::string>& argv) {
    const auto status = os::run(argv);
    if (!status) {
        return std::unexpected(status.error().message);
    }
    if (*status != 0) {
        return std::unexpected(std::format("{} failed with status {}", argv.front(), *status));
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
        why = "only root can take a snapshot";
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
    for (const auto& repository : settings.repositories) {
        found.push_back({.path = repository, .read_only = true});
    }
    return ordered(std::move(found));
}

std::vector<std::string> enter_command(const std::filesystem::path& egraph,
                                       const std::filesystem::path& root,
                                       std::span<const os::Bind> binds,
                                       std::span<const std::string> argv) {
    std::vector<std::string> command{egraph.string(), "enter"};
    for (const auto& bind : binds) {
        command.insert(command.end(),
                       {bind.read_only ? "--read-only" : "--bind", bind.path.string()});
    }
    command.push_back(root.string());
    command.emplace_back("--");
    command.insert(command.end(), argv.begin(), argv.end());
    return command;
}

std::vector<std::string> show_command(const std::filesystem::path& path) {
    return {"btrfs", "subvolume", "show", path.string()};
}

std::vector<std::string> create_command(const std::filesystem::path& source,
                                        const std::filesystem::path& destination) {
    return {"btrfs", "subvolume", "snapshot", source.string(), destination.string()};
}

std::vector<std::string> remove_command(const std::filesystem::path& path) {
    return {"btrfs", "subvolume", "delete", path.string()};
}

bool snapshottable(const std::filesystem::path& path) {
    // show prints the subvolume's details; only its status matters here.
    return os::run(show_command(path), std::filesystem::path{"/dev/null"}).value_or(1) == 0;
}

std::expected<void, std::string> create(const std::filesystem::path& source,
                                        const std::filesystem::path& destination) {
    return succeeded(create_command(source, destination));
}

std::expected<void, std::string> remove(const std::filesystem::path& destination) {
    return succeeded(remove_command(destination));
}

} // namespace egraph::snapshot
