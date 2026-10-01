#pragma once

// Actions: a plan shown, a yes asked for, emerge run on the plan.

#include "verify.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace egraph {

// Of EMERGE_DEFAULT_OPTS as portage splits it, the options that change how emerge carries out
// a plan but not what it plans: --jobs, --load-average, --keep-going and the like, each as
// --name or --name=value.
[[nodiscard]] std::vector<std::string> execution_options(std::span<const std::string> defaults);

// emerge's options and arguments to carry out request, asking nothing: EMERGE_DEFAULT_OPTS
// ignored but for passed (execution_options), --oneshot when oneshot.
[[nodiscard]] std::vector<std::string> run_arguments(const EmergeRequest& request, bool oneshot,
                                                     std::span<const std::string> passed);

// What an action knows once its plan is shown.
struct Readiness {
    bool refused = false;
    // Nothing to merge or uninstall.
    bool empty = false;
    // The installed packages' database can be written.
    bool writable = false;
    // --yes: run without asking.
    bool yes = false;
    // A question can be asked on a terminal.
    bool can_ask = false;
};

// Why an action stops before asking emerge to verify its plan.
enum class Stop : std::uint8_t {
    // emerge would refuse the plan.
    refused,
    nothing,
    unprivileged,
    // Neither --yes nor a terminal to ask on.
    unconfirmed,
};

[[nodiscard]] std::optional<Stop> stop_before_verifying(const Readiness& readiness);

} // namespace egraph
