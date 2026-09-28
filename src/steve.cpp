#include "steve.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>

namespace egraph::steve {

namespace {

template <class T> std::optional<T> number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

// steve takes a limit of 0 or below as none.
std::optional<std::int64_t> limit(std::optional<std::int64_t> value) {
    return value && *value > 0 ? value : std::nullopt;
}

std::optional<double> limit(std::optional<double> value) {
    return value && *value > 0 ? value : std::nullopt;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

struct Option {
    std::string_view name;
    char letter;
    bool takes_value;
};

// steve's options, as its getopt_long table has them.
constexpr std::array<Option, 12> options{{
    {.name = "help", .letter = 'h', .takes_value = false},
    {.name = "version", .letter = 'V', .takes_value = false},
    {.name = "jobs", .letter = 'j', .takes_value = true},
    {.name = "load-average", .letter = 'l', .takes_value = true},
    {.name = "load-recheck-timeout", .letter = 'r', .takes_value = true},
    {.name = "min-memory-avail", .letter = 'a', .takes_value = true},
    {.name = "min-jobs", .letter = 'm', .takes_value = true},
    {.name = "per-process-limit", .letter = 'p', .takes_value = true},
    {.name = "dev-name", .letter = '\0', .takes_value = true},
    {.name = "user", .letter = 'u', .takes_value = true},
    {.name = "verbose", .letter = 'v', .takes_value = false},
    {.name = "debug", .letter = 'd', .takes_value = false},
}};

// A long option by its name or, as getopt_long allows, an unambiguous prefix of it.
const Option* long_option(std::string_view name) {
    const Option* found = nullptr;
    for (const auto& option : options) {
        if (option.name == name) {
            return &option;
        }
        if (option.name.starts_with(name)) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &option;
        }
    }
    return found;
}

const Option* short_option(char letter) {
    const auto found = std::ranges::find(options, letter, &Option::letter);
    return found == options.end() || letter == '\0' ? nullptr : &*found;
}

// Applies one option's value; false when the value is not a number steve would take.
bool apply(Settings& settings, char letter, std::string_view value) {
    switch (letter) {
    case 'j':
        settings.jobs = number<std::int64_t>(value);
        return settings.jobs.has_value();
    case 'm':
        settings.min_jobs = number<std::int64_t>(value);
        return settings.min_jobs.has_value();
    case 'a':
        settings.min_memory = number<std::int64_t>(value);
        return settings.min_memory.has_value();
    case 'p':
        settings.per_process = number<std::int64_t>(value);
        return settings.per_process.has_value();
    case 'l':
        settings.load_average = number<double>(value);
        return settings.load_average.has_value();
    case 'r':
        settings.recheck_timeout = number<double>(value);
        return settings.recheck_timeout.has_value();
    default:
        return true;
    }
}

} // namespace

std::vector<std::string> get_arguments() {
    return {"stevie",
            "--get-tokens",
            "--get-jobs",
            "--get-min-jobs",
            "--get-load-average",
            "--get-load-recheck-timeout",
            "--get-min-memory-avail",
            "--get-per-process-limit"};
}

std::expected<Settings, std::string> parse_get(std::string_view output) {
    std::vector<std::string_view> lines;
    for (const auto line : std::views::split(output, '\n')) {
        if (!line.empty()) {
            lines.emplace_back(line.begin(), line.end());
        }
    }
    if (lines.size() != get_arguments().size() - 1) {
        return std::unexpected(std::string{output});
    }
    const auto tokens = number<std::int64_t>(lines.at(0));
    const auto jobs = number<std::int64_t>(lines.at(1));
    const auto min_jobs = number<std::int64_t>(lines.at(2));
    const auto load_average = number<double>(lines.at(3));
    const auto recheck_timeout = number<double>(lines.at(4));
    const auto min_memory = number<std::int64_t>(lines.at(5));
    const auto per_process = number<std::int64_t>(lines.at(6));
    if (!tokens || !jobs || !min_jobs || !load_average || !recheck_timeout || !min_memory ||
        !per_process) {
        return std::unexpected(std::string{output});
    }
    return Settings{.tokens = tokens,
                    .jobs = jobs,
                    .min_jobs = min_jobs,
                    .load_average = limit(load_average),
                    .recheck_timeout = recheck_timeout,
                    .min_memory = limit(min_memory),
                    .per_process = limit(per_process)};
}

std::vector<std::string> set_arguments(Setting setting, double value) {
    const auto whole = std::format("{}", std::llround(value));
    switch (setting) {
    case Setting::jobs:
        return {"stevie", "--set-jobs", whole};
    case Setting::min_jobs:
        return {"stevie", "--set-min-jobs", whole};
    case Setting::load_average:
        return {"stevie", "--set-load-average", std::format("{}", value)};
    case Setting::min_memory:
        return {"stevie", "--set-min-memory-avail", whole};
    case Setting::per_process:
        return {"stevie", "--set-per-process-limit", whole};
    case Setting::recheck_timeout:
        return {"stevie", "--set-load-recheck-timeout", std::format("{}", value)};
    }
    return {};
}

std::optional<Settings> parse_command_line(const std::vector<std::string>& argv) {
    if (argv.empty() || std::filesystem::path{argv.front()}.filename() != "steve") {
        return std::nullopt;
    }
    // steve's own defaults; jobs unset is one per CPU.
    Settings settings{.min_jobs = 1, .recheck_timeout = 0.5};
    for (std::size_t at = 1; at < argv.size(); ++at) {
        const std::string_view arg = argv.at(at);
        if (arg == "--") {
            break;
        }
        if (arg.starts_with("--")) {
            const auto equals = arg.find('=');
            const auto* option = long_option(arg.substr(2, equals - 2));
            if (option == nullptr || !option->takes_value) {
                continue;
            }
            std::string_view value;
            if (equals != std::string_view::npos) {
                value = arg.substr(equals + 1);
            } else if (++at < argv.size()) {
                value = argv.at(at);
            } else {
                return std::nullopt;
            }
            if (!apply(settings, option->letter, value)) {
                return std::nullopt;
            }
        } else if (arg.starts_with('-') && arg.size() > 1) {
            // Letters run together until one that takes a value, which is the rest or the next.
            for (std::size_t letter = 1; letter < arg.size(); ++letter) {
                const auto* option = short_option(arg.at(letter));
                if (option == nullptr || !option->takes_value) {
                    continue;
                }
                std::string_view value = arg.substr(letter + 1);
                if (value.empty()) {
                    if (++at >= argv.size()) {
                        return std::nullopt;
                    }
                    value = argv.at(at);
                }
                if (!apply(settings, option->letter, value)) {
                    return std::nullopt;
                }
                break;
            }
        }
    }
    settings.jobs = limit(settings.jobs);
    settings.load_average = limit(settings.load_average);
    settings.min_memory = limit(settings.min_memory);
    settings.per_process = limit(settings.per_process);
    return settings;
}

std::optional<std::vector<std::string>> find_command_line(const std::filesystem::path& proc) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{proc, error}) {
        const auto name = entry.path().filename().string();
        if (name.empty() ||
            !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        if (read_text(entry.path() / "comm") != "steve\n") {
            continue;
        }
        std::vector<std::string> argv;
        for (const auto arg : std::views::split(read_text(entry.path() / "cmdline"), '\0')) {
            argv.emplace_back(arg.begin(), arg.end());
        }
        // cmdline ends each argument with a NUL, the last one too.
        if (!argv.empty() && argv.back().empty()) {
            argv.pop_back();
        }
        if (!argv.empty()) {
            return argv;
        }
    }
    return std::nullopt;
}

std::optional<double> step(Setting setting, const Settings& settings, int direction,
                           std::size_t cpus) {
    const auto up = direction > 0;
    switch (setting) {
    case Setting::jobs: {
        const auto jobs = settings.jobs.value_or(static_cast<std::int64_t>(cpus));
        if (!up && jobs <= 1) {
            return std::nullopt;
        }
        return static_cast<double>(up ? jobs + 1 : jobs - 1);
    }
    case Setting::min_jobs: {
        const auto min_jobs = settings.min_jobs.value_or(1);
        if (up ? min_jobs >= settings.jobs.value_or(static_cast<std::int64_t>(cpus))
               : min_jobs <= 0) {
            return std::nullopt;
        }
        return static_cast<double>(up ? min_jobs + 1 : min_jobs - 1);
    }
    case Setting::load_average:
        if (!settings.load_average) {
            return up ? std::optional<double>{static_cast<double>(std::max<std::size_t>(cpus, 1))}
                      : std::nullopt;
        }
        if (!up && *settings.load_average < 2) {
            return std::nullopt;
        }
        return *settings.load_average + (up ? 1 : -1);
    case Setting::min_memory: {
        constexpr std::int64_t mib_step = 256;
        const auto memory = settings.min_memory.value_or(0);
        if (!up && memory <= 0) {
            return std::nullopt;
        }
        return static_cast<double>(up ? memory + mib_step
                                      : std::max<std::int64_t>(memory - mib_step, 0));
    }
    case Setting::per_process: {
        const auto per_process = settings.per_process.value_or(0);
        if (!up && per_process <= 0) {
            return std::nullopt;
        }
        return static_cast<double>(up ? per_process + 1 : per_process - 1);
    }
    case Setting::recheck_timeout: {
        const auto timeout = settings.recheck_timeout.value_or(0.5);
        const auto next = std::round((timeout + (up ? 0.1 : -0.1)) * 10) / 10;
        if (next < 0.1) {
            return std::nullopt;
        }
        return next;
    }
    }
    return std::nullopt;
}

} // namespace egraph::steve
