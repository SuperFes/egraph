#include "action.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <iterator>
#include <string_view>

namespace egraph {

namespace {

enum class Value : std::uint8_t {
    none,
    // y or n, perhaps as the next word.
    yes_no,
    // A number, perhaps as the next word.
    number,
    // Always the next word, if not given with "=".
    required,
};

struct Option {
    std::string_view name;
    Value value;
    bool passed;
};

// emerge's options that take a value, and the execution options passed on; any other option is
// taken to stand alone and dropped.
constexpr std::array options{
    Option{.name = "--alert", .value = Value::yes_no, .passed = true},
    Option{.name = "--buildpkg", .value = Value::yes_no, .passed = true},
    Option{.name = "--buildpkg-exclude", .value = Value::required, .passed = true},
    Option{.name = "--color", .value = Value::yes_no, .passed = true},
    Option{.name = "--fail-clean", .value = Value::yes_no, .passed = true},
    Option{.name = "--jobs", .value = Value::number, .passed = true},
    Option{.name = "--jobs-tmpdir-require-free-gb", .value = Value::number, .passed = true},
    Option{.name = "--keep-going", .value = Value::yes_no, .passed = true},
    Option{.name = "--load-average", .value = Value::number, .passed = true},
    Option{.name = "--nospinner", .value = Value::none, .passed = true},
    Option{.name = "--quiet", .value = Value::yes_no, .passed = true},
    Option{.name = "--quiet-build", .value = Value::yes_no, .passed = true},
    Option{.name = "--quiet-fail", .value = Value::yes_no, .passed = true},
    // Dropped, but their values must go with them.
    Option{.name = "--ask", .value = Value::yes_no, .passed = false},
    Option{.name = "--backtrack", .value = Value::number, .passed = false},
    Option{.name = "--exclude", .value = Value::required, .passed = false},
    Option{.name = "--usepkg-exclude", .value = Value::required, .passed = false},
    Option{.name = "--useoldpkg-atoms", .value = Value::required, .passed = false},
    Option{.name = "--rebuild-exclude", .value = Value::required, .passed = false},
    Option{.name = "--rebuild-ignore", .value = Value::required, .passed = false},
    Option{.name = "--reinstall-atoms", .value = Value::required, .passed = false},
    Option{.name = "--nousepkg-atoms", .value = Value::required, .passed = false},
    Option{.name = "--with-bdeps", .value = Value::yes_no, .passed = false},
};

// Short options passed on, as their long names.
constexpr std::array<std::pair<char, std::string_view>, 3> short_options{{
    {'b', "--buildpkg"},
    {'j', "--jobs"},
    {'q', "--quiet"},
}};

bool is_number(std::string_view word) {
    return !word.empty() &&
           std::ranges::all_of(word, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

bool fits(Value value, std::string_view word) {
    switch (value) {
    case Value::none:
        return false;
    case Value::yes_no:
        return word == "y" || word == "n";
    case Value::number:
        return is_number(word);
    case Value::required:
        return true;
    }
    return false;
}

} // namespace

std::vector<std::string> execution_options(std::span<const std::string> given) {
    const std::vector<std::string> defaults(given.begin(), given.end());
    std::vector<std::string> passed;
    // Whether the word at next is a value of value, and so consumed.
    const auto takes = [&](std::size_t next, Value value) {
        return next < defaults.size() && fits(value, defaults.at(next));
    };
    for (std::size_t at = 0; at < defaults.size(); ++at) {
        const std::string_view word = defaults.at(at);
        if (word.starts_with("--")) {
            const auto equals = word.find('=');
            const auto name = word.substr(0, equals);
            const auto option = std::ranges::find(options, name, &Option::name);
            if (option == options.end()) {
                continue;
            }
            std::string written{word};
            if (equals == std::string_view::npos && takes(at + 1, option->value)) {
                written = std::format("{}={}", name, defaults.at(++at));
            }
            if (option->passed) {
                passed.push_back(std::move(written));
            }
        } else if (word.starts_with('-')) {
            for (std::size_t letter = 1; letter < word.size(); ++letter) {
                const auto found = std::ranges::find(short_options, word.at(letter),
                                                     &std::pair<char, std::string_view>::first);
                if (found == short_options.end()) {
                    continue;
                }
                if (found->first != 'j') {
                    passed.emplace_back(found->second);
                    continue;
                }
                const auto count = word.substr(letter + 1);
                if (is_number(count)) {
                    passed.push_back(std::format("--jobs={}", count));
                } else if (count.empty() && takes(at + 1, Value::number)) {
                    passed.push_back(std::format("--jobs={}", defaults.at(++at)));
                } else {
                    passed.emplace_back("--jobs");
                }
                break;
            }
        }
    }
    return passed;
}

std::optional<std::uint32_t> jobs_of(std::span<const std::string> passed) {
    std::optional<std::uint32_t> jobs = 1;
    for (const std::string_view option : passed) {
        if (option == "--jobs") {
            jobs.reset();
        } else if (option.starts_with("--jobs=")) {
            std::uint32_t count = 0;
            const auto digits = option.substr(std::string_view{"--jobs="}.size());
            if (std::from_chars(digits.begin(), digits.end(), count).ec == std::errc{}) {
                jobs = count;
            }
        }
    }
    return jobs;
}

bool keep_going_of(std::span<const std::string> passed) {
    bool keep_going = false;
    for (const std::string_view option : passed) {
        if (option == "--keep-going" || option == "--keep-going=y") {
            keep_going = true;
        } else if (option == "--keep-going=n") {
            keep_going = false;
        }
    }
    return keep_going;
}

std::uint64_t tmpdir_free_gb_of(std::span<const std::string> passed) {
    constexpr std::string_view prefix = "--jobs-tmpdir-require-free-gb=";
    std::uint64_t gb = 18;
    for (const std::string_view option : passed) {
        if (option.starts_with(prefix)) {
            const auto digits = option.substr(prefix.size());
            std::uint64_t value = 0;
            if (std::from_chars(digits.begin(), digits.end(), value).ec == std::errc{}) {
                gb = value;
            }
        }
    }
    return gb;
}

std::vector<std::string> request_options(const EmergeRequest& request, bool oneshot) {
    std::vector<std::string> found;
    if (request.update) {
        found.emplace_back("--update");
    }
    if (request.deep) {
        found.emplace_back("--deep");
    }
    if (request.noreplace) {
        found.emplace_back("--noreplace");
    }
    if (request.rebuilds == UseRebuilds::all) {
        found.emplace_back("--newuse");
    } else if (request.rebuilds == UseRebuilds::changed) {
        found.emplace_back("--changed-use");
    }
    if (!request.dynamic_deps) {
        found.emplace_back("--dynamic-deps=n");
    }
    if (oneshot) {
        found.emplace_back("--oneshot");
    }
    return found;
}

std::vector<std::string> run_arguments(const EmergeRequest& request, bool oneshot,
                                       std::span<const std::string> passed) {
    std::vector<std::string> arguments{"--ignore-default-opts", "--ask=n"};
    arguments.insert(arguments.end(), passed.begin(), passed.end());
    std::ranges::move(request_options(request, oneshot), std::back_inserter(arguments));
    arguments.insert(arguments.end(), request.targets.begin(), request.targets.end());
    return arguments;
}

std::optional<Stop> stop_before_verifying(const Readiness& readiness) {
    if (readiness.refused) {
        return Stop::refused;
    }
    if (readiness.empty) {
        return Stop::nothing;
    }
    if (!readiness.writable) {
        return Stop::unprivileged;
    }
    if (!readiness.yes && !readiness.can_ask) {
        return Stop::unconfirmed;
    }
    return std::nullopt;
}

std::expected<RunSettings, std::string> parse_run_settings(std::string_view text) {
    using Json = nlohmann::json;
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    const auto words = json.find("options");
    if (words == json.end() || !words->is_array() ||
        !std::ranges::all_of(*words, &Json::is_string)) {
        return std::unexpected("options: not a list of words");
    }
    const auto elog = json.find("elog");
    if (elog == json.end() || !elog->is_object()) {
        return std::unexpected("elog: not an object");
    }
    RunSettings settings{.defaults = words->get<std::vector<std::string>>(),
                         .elog_summary = std::nullopt,
                         .elog_system = std::nullopt,
                         .jobserver = std::nullopt,
                         .tmpdir = {}};
    for (const auto& [field, value] : {std::pair{"summary", &settings.elog_summary},
                                       std::pair{"system", &settings.elog_system}}) {
        const auto found = elog->find(field);
        if (found == elog->end() || !(found->is_null() || found->is_string())) {
            return std::unexpected(std::format("elog: {}: not a string or null", field));
        }
        if (found->is_string()) {
            *value = found->get<std::string>();
        }
    }
    const auto jobserver = json.find("jobserver");
    if (jobserver == json.end() || !(jobserver->is_null() || jobserver->is_string())) {
        return std::unexpected("jobserver: not a string or null");
    }
    if (jobserver->is_string()) {
        settings.jobserver = jobserver->get<std::string>();
    }
    const auto tmpdir = json.find("tmpdir");
    if (tmpdir == json.end() || !tmpdir->is_string()) {
        return std::unexpected("tmpdir: not a string");
    }
    settings.tmpdir = tmpdir->get<std::string>();
    return settings;
}

} // namespace egraph
