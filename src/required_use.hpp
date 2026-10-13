#pragma once

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// REQUIRED_USE weighed as portage's check_required_use does.
struct RequiredUse {
    bool satisfied = true;
    // The constraints left unsatisfied, as the reduced tree's tounicode() prints them: whole
    // under ||, ^^ and ??, and inactive conditionals and satisfied terms dropped elsewhere.
    std::string unsatisfied;
};

// tokens are the string's split() and must be valid for the ebuild's EAPI and IUSE, as a
// visible ebuild's are; enabled holds the flags it would be built with. empty_true is the
// EAPI's empty_groups_always_true.
[[nodiscard]] RequiredUse check_required_use(std::span<const std::string_view> tokens,
                                             const std::set<std::string_view>& enabled,
                                             bool empty_true);

// What toggling one flag does to REQUIRED_USE.
struct Mend {
    // The constraints the toggle alone leaves unsatisfied, as RequiredUse words them; empty when
    // it leaves REQUIRED_USE satisfied, or finds it unsatisfied already.
    std::string broken;
    // Each smallest set of other toggles that satisfies it again, "flag" turning one on and
    // "-flag" off, each sorted and all in order; none when no set of up to three does.
    std::vector<std::vector<std::string>> mends;
};

// Toggling flag against enabled, the flags on now, mended only by toggling those in free.
[[nodiscard]] Mend mend_required_use(std::span<const std::string_view> tokens,
                                     const std::set<std::string_view>& enabled,
                                     std::span<const std::string_view> free, std::string_view flag,
                                     bool empty_true);

// portage's human_readable_required_use: the operators spelled out.
[[nodiscard]] std::string human_readable_required_use(std::string_view constraints);

} // namespace egraph
