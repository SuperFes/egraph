#pragma once

// Actions: a plan shown, a yes asked for, emerge run on the plan.

#include "verify.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// Of EMERGE_DEFAULT_OPTS as portage splits it, the options that change how emerge carries out
// a plan but not what it plans: --jobs, --load-average, --keep-going and the like, each as
// --name or --name=value.
[[nodiscard]] std::vector<std::string> execution_options(std::span<const std::string> defaults);

// How many builds emerge runs at once under passed (execution_options' spelling): the last
// --jobs decides, one without a number allowing any; one without --jobs.
[[nodiscard]] std::optional<std::uint32_t> jobs_of(std::span<const std::string> passed);

// Whether emerge goes on after a failure under passed: the last --keep-going decides, without a
// value or with y; off without one.
[[nodiscard]] bool keep_going_of(std::span<const std::string> passed);

// The GiB PORTAGE_TMPDIR must have free, beside 1 GiB per running build, before emerge starts
// another beside them, under passed: the last --jobs-tmpdir-require-free-gb, 18 by default; 0
// for no check.
[[nodiscard]] std::uint64_t tmpdir_free_gb_of(std::span<const std::string> passed);

// What emerge runs under, as egraph-build --emerge-options writes it.
struct RunSettings {
    // EMERGE_DEFAULT_OPTS as portage splits it.
    std::vector<std::string> defaults;
    // The log elog's save_summary module appends to, if it is on.
    std::optional<std::string> elog_summary;
    // PORTAGE_ELOG_SYSTEM without echo, for a run whose summary egraph shows instead; none to
    // leave it as it is.
    std::optional<std::string> elog_system;
    // The make jobserver's named pipe each build takes a token from, under
    // FEATURES=jobserver-token.
    std::optional<std::string> jobserver;
    // FEATURES=observability: a run publishes its status as emerge does.
    bool observability = false;
    // PORTAGE_TMPDIR, where builds run.
    std::string tmpdir;
};

[[nodiscard]] std::expected<RunSettings, std::string> parse_run_settings(std::string_view text);

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
