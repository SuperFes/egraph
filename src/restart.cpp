#include "restart.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace egraph {

bool Replacement::seen(std::optional<os::FileIdentity> file, Clock::time_point now) {
    if (!running_ || !file || *file == *running_) {
        seen_.reset();
        return false;
    }
    if (seen_ != file) {
        seen_ = file;
        since_ = now;
        return false;
    }
    return now - since_ >= settle;
}

std::optional<std::chrono::milliseconds> Replacement::wait(Clock::time_point now) const {
    if (!seen_) {
        return std::nullopt;
    }
    return std::max(std::chrono::milliseconds{0},
                    std::chrono::ceil<std::chrono::milliseconds>(since_ + settle - now));
}

void Replacement::keep() {
    if (seen_) {
        running_ = seen_;
        seen_.reset();
    }
}

SelfRestart SelfRestart::open() {
    auto path = os::executable();
    // The link reads so once the file is replaced; its path is where the new one is.
    constexpr std::string_view deleted = " (deleted)";
    if (auto text = path.string(); text.ends_with(deleted)) {
        text.resize(text.size() - deleted.size());
        path = std::move(text);
    }
    auto argv = os::command_line();
    const auto running = os::running_identity();
    if (path.empty() || !argv || !running) {
        return SelfRestart{{}, {}, std::nullopt};
    }
    return SelfRestart{std::move(path), std::move(*argv), *running};
}

std::optional<std::filesystem::path> SelfRestart::directory() const {
    if (path_.empty()) {
        return std::nullopt;
    }
    return path_.parent_path();
}

bool SelfRestart::due(Replacement::Clock::time_point now) {
    if (path_.empty()) {
        return false;
    }
    const auto file = os::identity(path_);
    return replacement_.seen(file ? std::optional{*file} : std::nullopt, now);
}

std::string SelfRestart::restart() {
    const auto error = os::exec(path_, argv_);
    replacement_.keep();
    return std::format("{}: {}", path_.string(), error.message());
}

} // namespace egraph
