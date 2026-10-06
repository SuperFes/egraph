#include "watch.hpp"

#include <algorithm>

namespace egraph {

std::vector<std::filesystem::path>
watch_directories(std::span<const std::span<const Input>> layers) {
    std::vector<std::filesystem::path> directories;
    for (const auto layer : layers) {
        for (const auto& input : layer) {
            const std::filesystem::path path{input.path};
            directories.push_back(input.kind == InputKind::directory ? path : path.parent_path());
        }
    }
    std::ranges::sort(directories);
    const auto [first, last] = std::ranges::unique(directories);
    directories.erase(first, last);
    return directories;
}

void Debounce::changed(Clock::time_point now) {
    if (!pending_) {
        pending_ = true;
        first_ = now;
    }
    last_ = now;
}

bool Debounce::due(Clock::time_point now) const {
    return pending_ && (now - last_ >= quiet || now - first_ >= longest);
}

std::optional<std::chrono::milliseconds> Debounce::wait(Clock::time_point now) const {
    if (!pending_) {
        return std::nullopt;
    }
    const auto until = std::min(last_ + quiet, first_ + longest);
    return std::max(std::chrono::milliseconds{0},
                    std::chrono::ceil<std::chrono::milliseconds>(until - now));
}

void Debounce::reset() {
    pending_ = false;
}

} // namespace egraph
