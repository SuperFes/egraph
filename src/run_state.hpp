#pragma once

// egraph exec's record of its last run on a root, in egraph's own state rather than emerge's
// mtimedb: what it was asked to do and what it merged, so that exec --resume can plan the same
// request again and leave out what is done.

#include "evaluated.hpp"
#include "exec.hpp"
#include "plan.hpp"
#include "store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

struct RunState {
    enum class Status : std::uint8_t { running, failed, done };
    // A package whose build or merge failed, and portage's log of it.
    struct Failure {
        std::string cpv{};
        std::string log{};
        bool operator==(const Failure&) const = default;
    };
    // exec's own arguments, its options and then its targets.
    std::vector<std::string> arguments{};
    // The cpvs it merged, and those of the runs it resumed, in merge order.
    std::vector<std::string> merged{};
    // This run's failures, in the order they happened; a record without any reads as none.
    std::vector<Failure> failed{};
    Status status = Status::running;
};

// Where the record of the last run on eroot is kept.
[[nodiscard]] std::filesystem::path run_state_path(const std::filesystem::path& eroot);

// The record as one JSON object, and back.
[[nodiscard]] std::string run_state_json(const RunState& state);
[[nodiscard]] std::expected<RunState, std::string> parse_run_state(std::string_view text);

// The steps left once a run that merged the cpvs merged is resumed: without the merges of those
// cpvs that are installed still. A step left out holds nothing back, as the schedule takes a
// merge without a step to be done.
[[nodiscard]] std::vector<Step> resumed_steps(const Store& store, const Evaluated& evaluated,
                                              const Plan& plan, std::vector<Step> steps,
                                              std::span<const std::string> merged);

} // namespace egraph
