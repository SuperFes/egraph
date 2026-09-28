#pragma once

// steve, the system-wide jobserver: its settings, read and changed through stevie, or read from
// its command line where /dev/steve is not open to us. Changes last until steve restarts.

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph::steve {

// Unset where it is not known, or for a limit, where there is none.
struct Settings {
    // Tokens free right now; only stevie knows.
    std::optional<std::int64_t> tokens{};
    std::optional<std::int64_t> jobs{};
    std::optional<std::int64_t> min_jobs{};
    std::optional<double> load_average{};
    // Seconds between load checks while over the limits.
    std::optional<double> recheck_timeout{};
    // MiB.
    std::optional<std::int64_t> min_memory{};
    std::optional<std::int64_t> per_process{};
};

enum class Setting : std::uint8_t {
    jobs,
    min_jobs,
    load_average,
    min_memory,
    per_process,
    recheck_timeout
};
inline constexpr std::array<Setting, 6> all_settings{
    Setting::jobs,       Setting::min_jobs,    Setting::load_average,
    Setting::min_memory, Setting::per_process, Setting::recheck_timeout};

// What is known of the running steve.
struct Status {
    // Read through stevie, so current, and open to changes.
    bool live = false;
    Settings settings{};
    // Why stevie could not read it, when it could not.
    std::string problem{};
};

// stevie's arguments that print every setting, one per line, and reading them back.
[[nodiscard]] std::vector<std::string> get_arguments();
[[nodiscard]] std::expected<Settings, std::string> parse_get(std::string_view output);

// stevie's arguments that change one setting.
[[nodiscard]] std::vector<std::string> set_arguments(Setting setting, double value);

// The settings steve's command line gives it (argv[0] first), or nothing if it is not steve's.
[[nodiscard]] std::optional<Settings> parse_command_line(const std::vector<std::string>& argv);

// The running steve's command line, found by its name under proc.
[[nodiscard]] std::optional<std::vector<std::string>>
find_command_line(const std::filesystem::path& proc = "/proc");

// A setting's value one step up (direction > 0) or down, within what steve accepts, or nothing
// when it cannot move. An unset load average steps up to cpus.
[[nodiscard]] std::optional<double> step(Setting setting, const Settings& settings, int direction,
                                         std::size_t cpus);

} // namespace egraph::steve
