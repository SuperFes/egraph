#include "version.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace egraph {

namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Consumes the leading digits of text.
std::string_view take_digits(std::string_view& text) {
    std::size_t length = 0;
    while (length < text.size() && is_digit(text.at(length))) {
        ++length;
    }
    const auto digits = text.substr(0, length);
    text.remove_prefix(length);
    return digits;
}

bool take(std::string_view& text, std::string_view prefix) {
    if (!text.starts_with(prefix)) {
        return false;
    }
    text.remove_prefix(prefix.size());
    return true;
}

// Compares non-negative integers written in decimal, of any length.
int compare_numbers(std::string_view a, std::string_view b) {
    const auto strip = [](std::string_view digits) {
        const auto first = digits.find_first_not_of('0');
        return first == std::string_view::npos ? std::string_view{} : digits.substr(first);
    };
    a = strip(a);
    b = strip(b);
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    const auto order = a.compare(b);
    return (order > 0) - (order < 0);
}

int sign(int value) {
    return (value > 0) - (value < 0);
}

int suffix_value(SuffixKind kind) {
    switch (kind) {
    case SuffixKind::alpha:
        return -4;
    case SuffixKind::beta:
        return -3;
    case SuffixKind::pre:
        return -2;
    case SuffixKind::rc:
        return -1;
    case SuffixKind::p:
        return 0;
    }
    return 0;
}

// A numeric component of the version; -1 stands in for one the other side lacks.
struct Component {
    bool missing = false;
    std::string_view digits;
};

} // namespace

std::optional<Version> parse_version(std::string_view text) {
    Version version;
    version.text = std::string{text};
    auto rest = text;
    const auto major = take_digits(rest);
    if (major.empty()) {
        return std::nullopt;
    }
    version.major = std::string{major};
    while (rest.starts_with('.')) {
        rest.remove_prefix(1);
        const auto minor = take_digits(rest);
        if (minor.empty()) {
            return std::nullopt;
        }
        version.minors.emplace_back(minor);
    }
    if (!rest.empty() && rest.front() >= 'a' && rest.front() <= 'z') {
        version.letter = rest.front();
        rest.remove_prefix(1);
    }
    // Longest names first, so "pre" is not read as "p".
    constexpr std::array<std::pair<std::string_view, SuffixKind>, 5> names{{
        {"alpha", SuffixKind::alpha},
        {"beta", SuffixKind::beta},
        {"pre", SuffixKind::pre},
        {"rc", SuffixKind::rc},
        {"p", SuffixKind::p},
    }};
    while (take(rest, "_")) {
        const auto* found = std::ranges::find_if(
            names, [&rest](const auto& name) { return rest.starts_with(name.first); });
        if (found == names.end()) {
            return std::nullopt;
        }
        rest.remove_prefix(found->first.size());
        version.suffixes.push_back(
            {.kind = found->second, .number = std::string{take_digits(rest)}});
    }
    version.base = std::string{text.substr(0, text.size() - rest.size())};
    if (take(rest, "-r")) {
        const auto revision = take_digits(rest);
        if (revision.empty()) {
            return std::nullopt;
        }
        version.revision = std::string{revision};
    }
    if (!rest.empty()) {
        return std::nullopt;
    }
    return version;
}

int vercmp(const Version& a, const Version& b) {
    if (a.text == b.text) {
        return 0;
    }
    if (const auto order = compare_numbers(a.major, b.major); order != 0) {
        return order;
    }
    const auto minors = std::max(a.minors.size(), b.minors.size());
    for (std::size_t i = 0; i < minors; ++i) {
        const Component x = i < a.minors.size()
                                ? Component{.missing = false, .digits = a.minors.at(i)}
                                : Component{.missing = true, .digits = {}};
        const Component y = i < b.minors.size()
                                ? Component{.missing = false, .digits = b.minors.at(i)}
                                : Component{.missing = true, .digits = {}};
        if (x.missing || y.missing) {
            // The present one is at least 0, so it wins.
            return x.missing ? -1 : 1;
        }
        int order = 0;
        if (x.digits.front() != '0' && y.digits.front() != '0') {
            order = compare_numbers(x.digits, y.digits);
        } else {
            // A leading zero makes the component a fraction: 1.02 < 1.1. Padding both to the same
            // length and comparing the digits is what portage does.
            const auto width = std::max(x.digits.size(), y.digits.size());
            std::string left{x.digits};
            std::string right{y.digits};
            left.resize(width, '0');
            right.resize(width, '0');
            order = sign(left.compare(right));
        }
        if (order != 0) {
            return order;
        }
    }
    if (a.letter != b.letter) {
        // A letter extends the list, so having one beats having none.
        if (a.letter == 0 || b.letter == 0) {
            return a.letter == 0 ? -1 : 1;
        }
        return a.letter < b.letter ? -1 : 1;
    }
    // A missing suffix counts as _p with number -1, so 1 < 1_p0.
    const auto suffixes = std::max(a.suffixes.size(), b.suffixes.size());
    for (std::size_t i = 0; i < suffixes; ++i) {
        const bool has_x = i < a.suffixes.size();
        const bool has_y = i < b.suffixes.size();
        const auto kind_x = has_x ? a.suffixes.at(i).kind : SuffixKind::p;
        const auto kind_y = has_y ? b.suffixes.at(i).kind : SuffixKind::p;
        if (kind_x != kind_y) {
            return suffix_value(kind_x) < suffix_value(kind_y) ? -1 : 1;
        }
        if (has_x != has_y) {
            // The missing side is -1; the present one is at least 0.
            return has_x ? 1 : -1;
        }
        if (const auto order = compare_numbers(a.suffixes.at(i).number, b.suffixes.at(i).number);
            order != 0) {
            return order;
        }
    }
    return compare_numbers(a.revision, b.revision);
}

} // namespace egraph
