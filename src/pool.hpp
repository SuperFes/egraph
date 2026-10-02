#pragma once

// The workers a parallel run talks to, started as it needs them and heard from together.

#include "os.hpp"
#include "schedule.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// Where builds run, and the GiB free there that one more beside others needs, as emerge's
// --jobs-tmpdir-require-free-gb; 0 for no check.
struct BuildRoom {
    std::filesystem::path tmpdir;
    std::uint64_t free_gb = 0;
};

// egraph-build --worker processes, as run_schedule's Pool.
class WorkerPool {
  public:
    // argv: how to start a worker. With a jobserver, each build takes a token from it; with
    // implicit_token, the first is held already, as under a make that started this process.
    // Why builds go one at a time for want of room is said once on notes.
    WorkerPool(std::vector<std::string> argv, std::optional<os::Jobserver> jobserver,
               bool implicit_token, BuildRoom room, std::ostream& notes EGRAPH_KEPT_BY_THIS);

    // Whether a build may start beside running others, as emerge weighs it: room's GiB free in
    // its tmpdir's portage directory, and 1 GiB for each running.
    [[nodiscard]] bool room_for(std::uint32_t running);

    // Starts a worker: its index.
    [[nodiscard]] std::expected<std::size_t, std::string> add();
    // Sends the worker a line; false once it takes no more.
    [[nodiscard]] bool send(std::size_t worker, std::string_view line);
    // A token for step's build: false when none is free yet.
    [[nodiscard]] std::expected<bool, std::string> take_token(std::size_t step);
    // Gives back the token step took.
    [[nodiscard]] std::expected<void, std::string> give_token(std::size_t step);
    // Waits for a worker's next line, or its end (reported once); with for_token, also for the
    // jobserver to have a token perhaps.
    [[nodiscard]] std::expected<Heard, std::string> next(bool for_token);
    // Closes each worker's input and waits for it: the first exit status not 0, else 0.
    [[nodiscard]] std::expected<int, std::string> finish();

  private:
    // Keeps the first way a worker went wrong: an exit status not 0, or why there is none.
    void record(const std::expected<int, os::SpawnError>& ended);

    std::vector<std::string> argv_;
    BuildRoom room_;
    std::reference_wrapper<std::ostream> notes_;
    bool told_room_ = false;
    std::optional<os::Jobserver> jobserver_;
    bool implicit_token_ = false;
    // By step, the token its build took; none for the implicit one.
    std::map<std::size_t, std::optional<std::byte>> tokens_;
    // The workers still running, and the index of each.
    std::vector<os::Talk> talks_;
    std::vector<std::size_t> indices_;
    std::size_t added_ = 0;
    std::deque<Heard> heard_;
    // How the workers that ended went, as record() keeps it.
    std::optional<std::expected<int, std::string>> ended_;
};

} // namespace egraph
