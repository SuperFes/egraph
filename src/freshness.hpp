#pragma once

#include "evaluated.hpp"
#include "store.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace egraph {

// Timestamps are coarse, so an input modified this close to the start of a build may have changed
// again within the same tick; such inputs are never trusted as unchanged. Matches the builder.
inline constexpr std::uint64_t racy_window_ns = 1'000'000'000;

// Why inputs recorded by a build started at build_time_ns no longer stat the same, if they do not.
[[nodiscard]] std::optional<std::string> staleness(std::span<const Input> inputs,
                                                   std::uint64_t build_time_ns);

// Why the store no longer describes the system, or nothing if every input stats as recorded.
[[nodiscard]] std::optional<std::string> staleness(const Store& store);

// As above, and also when it was built against another installed store than installed.
[[nodiscard]] std::optional<std::string> staleness(const Evaluated& evaluated,
                                                   const Store& installed);

// Either store's reason.
[[nodiscard]] std::optional<std::string> staleness(const Stores& stores);

} // namespace egraph
