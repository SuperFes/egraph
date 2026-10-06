#pragma once

#include "evaluated.hpp"
#include "repository.hpp"
#include "store.hpp"

#include <chrono>
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

[[nodiscard]] std::optional<std::string> staleness(const RepositoryIndex& index);

// Either store's reason.
[[nodiscard]] std::optional<std::string> staleness(const Stores& stores);

// How long a build starting at now_ns should wait for the store it writes to trust its inputs:
// until the newest of them, as they stat now, is older than the racy window. At most the window,
// for an input dated in the future. A refresh right after an emerge would otherwise write a store
// the next query refreshes again.
[[nodiscard]] std::chrono::nanoseconds settle_wait(std::span<const Input> inputs,
                                                   std::uint64_t now_ns);
[[nodiscard]] std::chrono::nanoseconds settle_wait(const Store& store, std::uint64_t now_ns);
[[nodiscard]] std::chrono::nanoseconds settle_wait(const RepositoryIndex& index,
                                                   std::uint64_t now_ns);
[[nodiscard]] std::chrono::nanoseconds settle_wait(const Stores& stores, std::uint64_t now_ns);

} // namespace egraph
