#pragma once

// watch and notify restart themselves when their executable is replaced (an upgrade), keeping
// their pid.

#include "os.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace egraph {

// Whether the running executable was replaced: its path names another file than the one
// running, and that file has gone settle unchanged.
class Replacement {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds settle{1000};

    explicit Replacement(std::optional<os::FileIdentity> running) : running_{running} {}

    // The path as it is now (none while it is missing); whether to restart.
    bool seen(std::optional<os::FileIdentity> file, Clock::time_point now);
    // How long until a replacement seen settles; none without one.
    [[nodiscard]] std::optional<std::chrono::milliseconds> wait(Clock::time_point now) const;
    // A restart failed: the file it would have run counts as the one running.
    void keep();

  private:
    std::optional<os::FileIdentity> running_;
    std::optional<os::FileIdentity> seen_;
    Clock::time_point since_{};
};

// The running executable, watched for its replacement, and run again over this process. Never
// due when /proc does not say what is running.
class SelfRestart {
  public:
    [[nodiscard]] static SelfRestart open();

    // The directory to watch, aside, for the executable's replacement.
    [[nodiscard]] std::optional<std::filesystem::path> directory() const;
    // Whether to restart, looking at the path now.
    bool due(Replacement::Clock::time_point now);
    [[nodiscard]] std::optional<std::chrono::milliseconds>
    wait(Replacement::Clock::time_point now) const {
        return replacement_.wait(now);
    }
    // The executable run again with the arguments this process had; returns only why not, after
    // which the replacement counts as running.
    std::string restart();

  private:
    SelfRestart(std::filesystem::path path, std::vector<std::string> argv,
                std::optional<os::FileIdentity> running)
        : path_{std::move(path)}, argv_{std::move(argv)}, replacement_{running} {}

    std::filesystem::path path_;
    std::vector<std::string> argv_;
    Replacement replacement_;
};

} // namespace egraph
