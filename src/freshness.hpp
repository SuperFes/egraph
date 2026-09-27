#pragma once

#include "store.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace egraph {

// Timestamps are coarse, so an input modified this close to the start of a build may have changed
// again within the same tick; such inputs are never trusted as unchanged. Matches the builder.
inline constexpr std::uint64_t racy_window_ns = 1'000'000'000;

// Why the store no longer describes the system, or nothing if every input stats as recorded.
[[nodiscard]] std::optional<std::string> staleness(const Store& store);

} // namespace egraph
