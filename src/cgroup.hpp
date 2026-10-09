#pragma once

// A cgroup v2 leaf per build, as portage's FEATURES="cgroup" makes them: egraph prepares a
// hierarchy of its own, the workers' portage places each build's phases in a leaf named for its
// cpv, and the kernel accounts the build's CPU, peak memory, IO and stalls exactly.

#include "pressure.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace egraph::cgroup {

// A leaf's counters since it was made; whatever the kernel does not keep is unset.
struct Counters {
    std::optional<std::uint64_t> cpu_usec{};
    std::optional<std::uint64_t> memory_peak{};
    std::optional<std::uint64_t> read_bytes{};
    std::optional<std::uint64_t> write_bytes{};
    pressure::StallTotals stalls{};
    bool operator==(const Counters&) const = default;
};

// A key's value in a flat keyed file such as cpu.stat ("usage_usec 123\n...").
[[nodiscard]] std::optional<std::uint64_t> parse_keyed(std::string_view text, std::string_view key);

// Bytes read and written over every device in io.stat.
struct Io {
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    bool operator==(const Io&) const = default;
};
[[nodiscard]] std::optional<Io> parse_io_stat(std::string_view text);

// The leaf portage names for a cpv (portage.util.cgroup.sanitize_name).
[[nodiscard]] std::string leaf_name(std::string_view cpv);

// The leaf's counters, none once it is gone.
[[nodiscard]] std::optional<Counters> read_counters(const std::filesystem::path& leaf);

// Kills what is left in the cgroup and below it, and removes them, as far as the kernel lets.
void remove(const std::filesystem::path& cgroup);

// egraph's hierarchy for one run, <cgroupfs>/egraph/exec-<pid>, with the cpu, io and memory
// controllers its leaves need; removed with whatever is left in it when it goes.
class Hierarchy {
  public:
    // Fails where cgroupfs is not cgroup v2 or cannot be written (egraph is not root).
    [[nodiscard]] static std::expected<Hierarchy, std::string>
    create(const std::filesystem::path& cgroupfs, std::int64_t pid,
           const std::filesystem::path& proc = "/proc");

    Hierarchy(const Hierarchy&) = delete;
    Hierarchy& operator=(const Hierarchy&) = delete;
    Hierarchy(Hierarchy&& other) noexcept;
    Hierarchy& operator=(Hierarchy&& other) noexcept;
    ~Hierarchy();

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::filesystem::path leaf(std::string_view cpv) const;

  private:
    explicit Hierarchy(std::filesystem::path root) : root_{std::move(root)} {}
    std::filesystem::path root_;
};

} // namespace egraph::cgroup
