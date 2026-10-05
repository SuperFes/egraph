#include "resume.hpp"

#include "action.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <thread>

namespace egraph {

using Json = nlohmann::json;

namespace {

// How emerge's parse_opts leaves an option's value.
enum class Kind : std::uint8_t {
    // A flag: true.
    flag,
    // True when on (no value, True or y), left out otherwise.
    on_or_out,
    // True when on, its value otherwise.
    on_or_value,
    // "y" when on, its value otherwise.
    y_or_value,
    // Its value.
    text,
    // A count, true when on, left out for n; 0 for every CPU.
    jobs,
    // A whole number.
    number,
    // A number above 0, left out otherwise.
    load,
    // A depth, true when on; left out below 0.
    deep,
};

struct Rule {
    std::string_view name;
    Kind kind;
};

constexpr std::array rules{
    Rule{.name = "--alert", .kind = Kind::on_or_out},
    Rule{.name = "--ask", .kind = Kind::on_or_out},
    Rule{.name = "--buildpkg", .kind = Kind::on_or_value},
    Rule{.name = "--color", .kind = Kind::text},
    Rule{.name = "--config-root", .kind = Kind::text},
    Rule{.name = "--deep", .kind = Kind::deep},
    Rule{.name = "--dynamic-deps", .kind = Kind::text},
    Rule{.name = "--fail-clean", .kind = Kind::on_or_value},
    Rule{.name = "--ignore-default-opts", .kind = Kind::flag},
    Rule{.name = "--jobs", .kind = Kind::jobs},
    Rule{.name = "--jobs-tmpdir-require-free-gb", .kind = Kind::number},
    Rule{.name = "--keep-going", .kind = Kind::on_or_out},
    Rule{.name = "--load-average", .kind = Kind::load},
    Rule{.name = "--newuse", .kind = Kind::flag},
    Rule{.name = "--noreplace", .kind = Kind::flag},
    Rule{.name = "--nospinner", .kind = Kind::flag},
    Rule{.name = "--oneshot", .kind = Kind::flag},
    Rule{.name = "--prefix", .kind = Kind::text},
    Rule{.name = "--quiet", .kind = Kind::on_or_out},
    Rule{.name = "--quiet-build", .kind = Kind::y_or_value},
    Rule{.name = "--quiet-fail", .kind = Kind::y_or_value},
    Rule{.name = "--reinstall", .kind = Kind::text},
    Rule{.name = "--root", .kind = Kind::text},
    Rule{.name = "--update", .kind = Kind::flag},
};

template <class Number> std::optional<Number> number_of(std::string_view text) {
    Number found{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), found);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return found;
}

// The value myopts holds for an option given as given, if any.
std::optional<Json> kept(Kind kind, const std::optional<std::string>& given) {
    // Without a value, as emerge's parser fills one in.
    const auto value = given.value_or("True");
    const bool on = value == "True" || value == "y";
    switch (kind) {
    case Kind::flag:
        return true;
    case Kind::on_or_out:
        return on ? std::optional<Json>{true} : std::nullopt;
    case Kind::on_or_value:
        return on ? Json(true) : Json(value);
    case Kind::y_or_value:
        return on ? Json("y") : Json(value);
    case Kind::text:
        return Json(value);
    case Kind::jobs: {
        if (on) {
            return true;
        }
        const auto count = number_of<std::int64_t>(value);
        if (!count) {
            return std::nullopt;
        }
        return *count == 0 ? Json(std::max(1U, std::thread::hardware_concurrency())) : Json(*count);
    }
    case Kind::number: {
        const auto found = number_of<std::int64_t>(value);
        return found ? std::optional<Json>{*found} : std::nullopt;
    }
    case Kind::load: {
        const auto found = number_of<double>(value);
        return found && *found > 0 ? std::optional<Json>{*found} : std::nullopt;
    }
    case Kind::deep: {
        if (on) {
            return true;
        }
        const auto depth = number_of<std::int64_t>(value);
        return depth && *depth >= 0 ? std::optional<Json>{*depth} : std::nullopt;
    }
    }
    return std::nullopt;
}

Json myopts_of(std::span<const std::string> options) {
    // The last of each decides.
    std::map<std::string, std::optional<std::string>, std::less<>> given;
    auto excludes = Json::array();
    for (const std::string_view option : options) {
        const auto equals = option.find('=');
        std::string name{option.substr(0, equals)};
        std::optional<std::string> value;
        if (equals != std::string_view::npos) {
            value = std::string{option.substr(equals + 1)};
        }
        if (name == "--buildpkg-exclude") {
            excludes.push_back(value.value_or(""));
        } else if (name == "--changed-use") {
            given.insert_or_assign("--reinstall", "changed-use");
        } else {
            given.insert_or_assign(std::move(name), std::move(value));
        }
    }
    Json found{{"--regex-search-auto", "y"}};
    if (!excludes.empty()) {
        found.emplace("--buildpkg-exclude", std::move(excludes));
    }
    for (const auto& [name, value] : given) {
        const auto rule = std::ranges::find(rules, name, &Rule::name);
        if (rule == rules.end()) {
            found.emplace(name, value ? Json(*value) : Json(true));
        } else if (auto each = kept(rule->kind, value)) {
            found.emplace(name, std::move(*each));
        }
    }
    return found;
}

std::string entry_of(const Evaluated& original, const Plan& plan, std::string_view eroot,
                     std::span<const std::size_t> listed, std::span<const std::string> options,
                     std::span<const std::string> favorites) {
    const auto& evaluated = plan.evaluated_or(original);
    auto mergelist = Json::array();
    for (const auto index : listed) {
        const auto& candidate = evaluated.candidates.at(plan.merges.at(index).candidate);
        mergelist.push_back({"ebuild", eroot, evaluated.string(candidate.cpv), "merge"});
    }
    return Json{{"binpkgs", Json::array()},
                {"favorites", Json(std::vector<std::string>(favorites.begin(), favorites.end()))},
                {"mergelist", std::move(mergelist)},
                {"myopts", myopts_of(options)}}
        .dump(1);
}

} // namespace

std::string emerge_myopts(std::span<const std::string> options) {
    return myopts_of(options).dump();
}

std::vector<std::string> resume_favorites(std::span<const std::string> targets,
                                          std::span<const Argument> arguments) {
    std::vector<std::string> found;
    for (const auto& target : targets) {
        if (target.starts_with('@')) {
            found.push_back(target);
        }
    }
    for (const auto& argument : arguments) {
        if (argument.set.empty()) {
            found.push_back(argument.atom);
        }
    }
    return found;
}

std::string resume_entry(const Evaluated& evaluated, const Plan& plan, std::string_view eroot,
                         const EmergeRequest& request, bool oneshot,
                         std::span<const Argument> arguments) {
    const std::vector<std::size_t> listed(plan.order.begin(), plan.order.end());
    const auto options = request_options(request, oneshot);
    const auto favorites = resume_favorites(request.targets, arguments);
    return entry_of(evaluated, plan, eroot, listed, options, favorites);
}

} // namespace egraph
