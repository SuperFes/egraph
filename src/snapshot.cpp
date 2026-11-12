#include "snapshot.hpp"

#include "os.hpp"

#include <format>
#include <limits>
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
