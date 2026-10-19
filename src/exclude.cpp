#include "exclude.hpp"

#include "version.hpp"

#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>

namespace egraph {

namespace {

std::expected<Exclusion, std::string> parse_exclusion(std::string_view text) {
    const auto invalid = [text] {
        return std::unexpected(std::format(
            "{}: only package names and slot atoms (with wildcards) are excluded", text));
    };
    // A package name without a category is one in any, as WildcardPackageSet reads it.
    const auto name = text.substr(0, text.find(':'));
    const auto spelled = name.contains('/') ? std::string(text) : std::format("*/{}", text);
    auto atom = parse_config_atom(spelled);
    if (!atom || atom->op != Operator::none || atom->repo || !atom->use.empty()) {
        return invalid();
    }
    return Exclusion{.text = std::string(text), .atom = std::move(*atom)};
}

void split_into(std::string_view value, std::vector<std::string_view>& words) {
    std::size_t at = 0;
    while (at < value.size()) {
        const auto start = value.find_first_not_of(" \t\r\n", at);
        if (start == std::string_view::npos) {
            return;
        }
        const auto end = std::min(value.find_first_of(" \t\r\n", start), value.size());
        words.push_back(value.substr(start, end - start));
        at = end;
    }
}

} // namespace

std::expected<std::vector<Exclusion>, std::string>
parse_exclusions(std::span<const std::string> values) {
    std::vector<std::string_view> words;
    for (const auto& value : values) {
        split_into(value, words);
    }
    std::vector<Exclusion> found;
    for (const auto word : words) {
        auto exclusion = parse_exclusion(word);
        if (!exclusion) {
            return std::unexpected(exclusion.error());
        }
        found.push_back(std::move(*exclusion));
    }
    return found;
}

std::expected<std::vector<Exclusion>, std::string> parse_exclude_list(std::string_view text) {
    std::vector<Exclusion> found;
    std::size_t number = 0;
    for (const auto line : std::views::split(text, '\n')) {
        ++number;
        auto entry = std::string_view{line.begin(), line.end()};
        entry = entry.substr(0, entry.find('#'));
        std::vector<std::string_view> words;
        split_into(entry, words);
        for (const auto word : words) {
            auto exclusion = parse_exclusion(word);
            if (!exclusion) {
                return std::unexpected(std::format("line {}: {}", number, exclusion.error()));
            }
            found.push_back(std::move(*exclusion));
        }
    }
    return found;
}

std::filesystem::path depclean_exclude_path(const std::filesystem::path& config_root) {
    return config_root / "etc/egraph/depclean-exclude";
}

std::expected<std::vector<Exclusion>, std::string>
read_exclude_list(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return std::vector<Exclusion>{};
    }
    std::ifstream in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    if (!in) {
        return std::unexpected(std::format("{}: cannot read it", path.string()));
    }
    auto found = parse_exclude_list(text.str());
    if (!found) {
        return std::unexpected(std::format("{}: {}", path.string(), found.error()));
    }
    return found;
}

bool excludes(const Store& store, const Package& pkg, const Exclusion& exclusion) {
    const auto cp = store.string(pkg.cp);
    const auto cpv = store.string(pkg.cpv);
    const auto version = parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
    return version && matches(exclusion.atom, cp, *version, store.string(pkg.slot),
                              store.string(pkg.sub_slot), store.string(pkg.repo));
}

} // namespace egraph
