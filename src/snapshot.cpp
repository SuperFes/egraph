#include "snapshot.hpp"

#include "os.hpp"

#include <format>
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
