#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

enum class SuffixKind : std::uint8_t { alpha, beta, pre, rc, p };

struct VersionSuffix {
    SuffixKind kind = SuffixKind::p;
    // The digits after the suffix name, possibly none.
    std::string number;
};

// A version as portage's ver_regexp reads it: 1.2.3b_rc4_p5-r6.
struct Version {
    std::string text;
    std::string major;
    std::vector<std::string> minors;
    // The letter after the numbers, or 0.
    char letter = 0;
    std::vector<VersionSuffix> suffixes;
    // The digits of -rN, or empty.
    std::string revision;
    // text without its -rN.
    std::string base;
};

[[nodiscard]] std::optional<Version> parse_version(std::string_view text);

// portage.versions.vercmp: negative, zero or positive. Not quite PMS: a missing numeric
// component sorts below 0, so 1.0.0 > 1.0.
[[nodiscard]] int vercmp(const Version& a, const Version& b);

} // namespace egraph
