#include "atom.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace egraph {

namespace {

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// [\w][\w<extra>]*, the shape portage gives categories, package names, slots and repos.
bool is_name(std::string_view text, std::string_view extra) {
    if (text.empty() || !is_word(text.front())) {
        return false;
    }
    return std::ranges::all_of(
        text, [extra](char c) { return is_word(c) || extra.find(c) != std::string_view::npos; });
}

bool is_flag(std::string_view text) {
    if (text.empty() || !(is_word(text.front()) && text.front() != '_')) {
        return false;
    }
    return std::ranges::all_of(
        text, [](char c) { return is_word(c) || c == '+' || c == '@' || c == '-'; });
}

// A package name may not end in something that reads as a version: foo-1 is foo at version 1.
bool ends_in_version(std::string_view name) {
    for (auto dash = name.find('-'); dash != std::string_view::npos;
         dash = name.find('-', dash + 1)) {
        if (parse_version(name.substr(dash + 1))) {
            return true;
        }
    }
    return false;
}

std::unexpected<std::string> invalid(std::string_view text, std::string_view why) {
    return std::unexpected(std::format("{}: invalid atom: {}", text, why));
}

std::expected<std::vector<UseDependency>, std::string> parse_use(std::string_view whole,
                                                                 std::string_view list) {
    std::vector<UseDependency> use;
    if (list.empty()) {
        return invalid(whole, "empty USE dependency");
    }
    while (true) {
        const auto comma = list.find(',');
        auto token = list.substr(0, comma);
        if (token.ends_with('?') || token.ends_with('=') || token.starts_with('!')) {
            return invalid(whole, "conditional USE dependencies need a parent package");
        }
        UseDependency dependency;
        if (token.starts_with('-')) {
            dependency.enabled = false;
            token.remove_prefix(1);
        }
        if (token.ends_with("(+)")) {
            dependency.fallback = UseDependency::Default::enabled;
            token.remove_suffix(3);
        } else if (token.ends_with("(-)")) {
            dependency.fallback = UseDependency::Default::disabled;
            token.remove_suffix(3);
        }
        if (!is_flag(token)) {
            return invalid(whole, std::format("bad USE flag '{}'", token));
        }
        dependency.flag = std::string{token};
        use.push_back(std::move(dependency));
        if (comma == std::string_view::npos) {
            return use;
        }
        list.remove_prefix(comma + 1);
    }
}

// The version text with its first component's leading zeros dropped, as portage compares globs.
std::string glob_key(const Version& version) {
    auto base = std::string_view{version.base};
    const auto first = base.find_first_not_of('0');
    base = first == std::string_view::npos ? std::string_view{} : base.substr(first);
    std::string key = base.empty() || base.front() < '0' || base.front() > '9' ? "0" : "";
    key += base;
    if (!version.revision.empty()) {
        key += "-r";
        key += version.revision;
    }
    return key;
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool glob_matches(const Version& pattern, const Version& candidate) {
    const auto prefix = glob_key(pattern);
    const auto key = glob_key(candidate);
    if (!key.starts_with(prefix)) {
        return false;
    }
    // Only at a boundary between version parts: 1* does not match 10.
    if (key.size() == prefix.size()) {
        return true;
    }
    const char next = key.at(prefix.size());
    return next == '.' || next == '_' || next == '-' || is_digit(prefix.back()) != is_digit(next);
}

bool contains(const std::vector<std::string>& items, std::string_view value) {
    return std::ranges::find(items, value) != items.end();
}

bool built_with(const Store& store, const Package& pkg, std::string_view flag) {
    return std::ranges::any_of(store.ids_in(pkg.use),
                               [&](std::uint32_t id) { return store.string(id) == flag; });
}

// Package._iuse.get_flag with vardb's implicit match: listed in IUSE, or implied.
bool in_iuse(const Store& store, const Package& pkg, std::string_view flag) {
    for (const auto id : store.ids_in(pkg.iuse)) {
        auto listed = store.string(id);
        if (listed.starts_with('+') || listed.starts_with('-')) {
            listed.remove_prefix(1);
        }
        if (listed == flag) {
            return true;
        }
    }
    const auto& implicit = store.implicit;
    if (pkg.iuse_effective) {
        // Built packages also count every flag they were built with.
        return contains(implicit.effective, flag) || built_with(store, pkg, flag);
    }
    return contains(implicit.literals, flag) ||
           std::ranges::any_of(implicit.prefixes, [flag](const std::string& prefix) {
               return flag.starts_with(prefix);
           });
}

bool use_matches(const Store& store, const Package& pkg, const std::vector<UseDependency>& use) {
    std::vector<std::string_view> enabled;
    std::vector<std::string_view> disabled;
    std::vector<std::string_view> missing_enabled;
    std::vector<std::string_view> missing_disabled;
    for (const auto& dependency : use) {
        const std::string_view flag = dependency.flag;
        const bool known = in_iuse(store, pkg, flag);
        if (dependency.fallback == UseDependency::Default::none && !known) {
            return false;
        }
        (dependency.enabled ? enabled : disabled).push_back(flag);
        if (!known && dependency.fallback == UseDependency::Default::enabled) {
            missing_enabled.push_back(flag);
        }
        if (!known && dependency.fallback == UseDependency::Default::disabled) {
            missing_disabled.push_back(flag);
        }
    }
    const auto in = [](const std::vector<std::string_view>& items, std::string_view flag) {
        return std::ranges::find(items, flag) != items.end();
    };
    // In USE, and a valid flag for the package.
    const auto set = [&](std::string_view flag) {
        return built_with(store, pkg, flag) && in_iuse(store, pkg, flag);
    };
    for (const auto flag : enabled) {
        if (in(missing_disabled, flag)) {
            return false;
        }
        if (!set(flag) && !in(missing_enabled, flag)) {
            return false;
        }
    }
    for (const auto flag : disabled) {
        if (in(missing_enabled, flag)) {
            return false;
        }
        if (set(flag) && !in(missing_disabled, flag)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::expected<Atom, std::string> parse_atom(std::string_view text) {
    if (text.starts_with('!')) {
        return invalid(text, "a blocker is not a query");
    }
    Atom atom;
    auto rest = text;
    if (rest.ends_with(']')) {
        const auto open = rest.rfind('[');
        if (open == std::string_view::npos) {
            return invalid(text, "unbalanced ']'");
        }
        auto use = parse_use(text, rest.substr(open + 1, rest.size() - open - 2));
        if (!use) {
            return std::unexpected(std::move(use.error()));
        }
        atom.use = std::move(*use);
        rest = rest.substr(0, open);
    }
    if (const auto colons = rest.find("::"); colons != std::string_view::npos) {
        const auto repo = rest.substr(colons + 2);
        if (!is_name(repo, "-")) {
            return invalid(text, "bad repository name");
        }
        atom.repo = std::string{repo};
        rest = rest.substr(0, colons);
    }
    if (const auto colon = rest.find(':'); colon != std::string_view::npos) {
        auto slot = rest.substr(colon + 1);
        rest = rest.substr(0, colon);
        // := and :* do not restrict the slot; := only means something inside a dependency.
        if (slot != "*" && slot != "=") {
            if (slot.ends_with('=')) {
                slot.remove_suffix(1);
            }
            const auto slash = slot.find('/');
            const auto main = slot.substr(0, slash);
            if (!is_name(main, "+.-")) {
                return invalid(text, "bad slot");
            }
            atom.slot = std::string{main};
            if (slash != std::string_view::npos) {
                const auto sub = slot.substr(slash + 1);
                if (!is_name(sub, "+.-")) {
                    return invalid(text, "bad sub-slot");
                }
                atom.sub_slot = std::string{sub};
            }
        }
    }

    constexpr std::array<std::pair<std::string_view, Operator>, 6> operators{{
        {">=", Operator::greater_equal},
        {"<=", Operator::less_equal},
        {">", Operator::greater},
        {"<", Operator::less},
        {"=", Operator::equal},
        {"~", Operator::approximately},
    }};
    for (const auto& [spelling, op] : operators) {
        if (rest.starts_with(spelling)) {
            atom.op = op;
            rest.remove_prefix(spelling.size());
            break;
        }
    }
    if (rest.ends_with('*')) {
        if (atom.op != Operator::equal) {
            return invalid(text, "a '*' version glob needs '='");
        }
        atom.op = Operator::glob;
        rest.remove_suffix(1);
    }

    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return invalid(text, "no category");
    }
    const auto category = rest.substr(0, slash);
    auto name = rest.substr(slash + 1);
    if (atom.op != Operator::none) {
        // The version starts at the first '-' after which the rest reads as one.
        std::optional<Version> version;
        for (auto dash = name.find('-'); dash != std::string_view::npos;
             dash = name.find('-', dash + 1)) {
            version = parse_version(name.substr(dash + 1));
            if (version) {
                name = name.substr(0, dash);
                break;
            }
        }
        if (!version) {
            return invalid(text, "an operator needs a version");
        }
        atom.version = std::move(version);
    }
    if (ends_in_version(name)) {
        return invalid(text, atom.op == Operator::none ? "a version needs an operator"
                                                       : "package name ends in a version");
    }
    if (!is_name(category, "+.-") || !is_name(name, "+-")) {
        return invalid(text, "bad category or package name");
    }
    atom.cp = std::format("{}/{}", category, name);
    return atom;
}

bool matches(const Store& store, const Package& pkg, const Atom& atom) {
    const auto cp = store.string(pkg.cp);
    if (cp != atom.cp) {
        return false;
    }
    if (atom.version) {
        const auto cpv = store.string(pkg.cpv);
        const auto version = parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
        if (!version) {
            return false;
        }
        const auto& wanted = *atom.version;
        switch (atom.op) {
        case Operator::none:
            break;
        case Operator::equal:
            if (vercmp(*version, wanted) != 0) {
                return false;
            }
            break;
        case Operator::glob:
            if (!glob_matches(wanted, *version)) {
                return false;
            }
            break;
        case Operator::approximately:
            if (version->base != wanted.base) {
                return false;
            }
            break;
        case Operator::less:
            if (vercmp(*version, wanted) >= 0) {
                return false;
            }
            break;
        case Operator::less_equal:
            if (vercmp(*version, wanted) > 0) {
                return false;
            }
            break;
        case Operator::greater:
            if (vercmp(*version, wanted) <= 0) {
                return false;
            }
            break;
        case Operator::greater_equal:
            if (vercmp(*version, wanted) < 0) {
                return false;
            }
            break;
        }
    }
    if (atom.slot && (store.string(pkg.slot) != *atom.slot ||
                      (atom.sub_slot && store.string(pkg.sub_slot) != *atom.sub_slot))) {
        return false;
    }
    if (atom.repo && store.string(pkg.repo) != *atom.repo) {
        return false;
    }
    return use_matches(store, pkg, atom.use);
}

} // namespace egraph
