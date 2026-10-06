#include "freshness.hpp"

#include "os.hpp"

#include <algorithm>
#include <format>
#include <system_error>

namespace egraph {

namespace {

bool is_absent(std::error_code error) {
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
}

bool same_kind(InputKind recorded, os::FileKind actual) {
    switch (recorded) {
    case InputKind::file:
        return actual == os::FileKind::file;
    case InputKind::directory:
        return actual == os::FileKind::directory;
    case InputKind::symlink:
        return actual == os::FileKind::symlink;
    case InputKind::missing:
        return false;
    }
    return false;
}

} // namespace

std::optional<std::string> staleness(std::span<const Input> inputs, std::uint64_t build_time_ns) {
    const auto racy_after = build_time_ns > racy_window_ns ? build_time_ns - racy_window_ns : 0;
    for (const auto& input : inputs) {
        const auto status = os::lstat(input.path);
        if (input.kind == InputKind::missing) {
            if (status) {
                return std::format("{}: created", input.path);
            }
            if (!is_absent(status.error())) {
                return std::format("{}: {}", input.path, status.error().message());
            }
            continue;
        }
        if (!status) {
            return std::format("{}: {}", input.path, status.error().message());
        }
        if (!same_kind(input.kind, status->kind) || status->mtime_ns != input.mtime_ns ||
            status->size != input.size) {
            return std::format("{}: changed", input.path);
        }
        if (input.mtime_ns >= racy_after) {
            return std::format("{}: modified too close to the build to trust", input.path);
        }
    }
    return std::nullopt;
}

std::optional<std::string> staleness(const Store& store) {
    return staleness(store.inputs, store.meta.build_time_ns);
}

std::optional<std::string> staleness(const Evaluated& evaluated, const Store& installed) {
    if (evaluated.meta.installed_build_time_ns != installed.meta.build_time_ns) {
        return std::string{"built against another installed store"};
    }
    return staleness(evaluated.inputs, evaluated.meta.build_time_ns);
}

std::optional<std::string> staleness(const Stores& stores) {
    if (auto reason = staleness(stores.installed)) {
        return reason;
    }
    return staleness(stores.evaluated, stores.installed);
}

std::chrono::nanoseconds settle_wait(std::span<const Input> inputs, std::uint64_t now_ns) {
    std::uint64_t newest = 0;
    for (const auto& input : inputs) {
        if (const auto status = os::lstat(input.path)) {
            newest = std::max(newest, status->mtime_ns);
        }
    }
    const auto settled = newest + racy_window_ns;
    if (settled <= now_ns) {
        return std::chrono::nanoseconds{0};
    }
    return std::chrono::nanoseconds{
        static_cast<std::int64_t>(std::min(settled - now_ns, racy_window_ns))};
}

std::chrono::nanoseconds settle_wait(const Store& store, std::uint64_t now_ns) {
    return settle_wait(store.inputs, now_ns);
}

std::chrono::nanoseconds settle_wait(const Stores& stores, std::uint64_t now_ns) {
    return std::max(settle_wait(stores.installed, now_ns),
                    settle_wait(stores.evaluated.inputs, now_ns));
}

} // namespace egraph
