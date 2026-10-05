#pragma once

// What egraph logs of the runs that change the system: to the systemd journal, with structured
// fields, and to its own file as JSON lines; never to emerge's log.

#include "store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace egraph::log {

// Where events go. automatic: the journal when egraph has it and systemd runs, else the file.
enum class Sink : std::uint8_t { automatic, journal, file, both, none };

struct Field {
    // Lower case with underscores: "cpv", "seconds".
    std::string name;
    std::variant<std::string, std::int64_t, double> value;
    bool operator==(const Field&) const = default;
};

struct Event {
    // Seconds since the epoch.
    double time = 0;
    // The run it belongs to, the same for each of a run's events.
    std::string run;
    // What happened: "run", "phase", "built", "merged", "failed", "end"...
    std::string kind;
    // As a person reads it.
    std::string message;
    // syslog's: 3 an error, 4 a warning, 6 information.
    int priority = 6;
    std::vector<Field> fields;
};

// The event as a line of the file, a JSON object with sorted keys and no newline: time, run,
// event, message, priority, and its fields.
[[nodiscard]] std::string file_line(const Event& event);

// The event as journal fields, NAME=value: MESSAGE, PRIORITY, SYSLOG_IDENTIFIER=egraph,
// EGRAPH_RUN, EGRAPH_EVENT, and each field as EGRAPH_ and its name in upper case.
[[nodiscard]] std::vector<std::string> journal_fields(const Event& event);

struct Targets {
    bool journal = false;
    bool file = false;
    bool operator==(const Targets&) const = default;
};

// Where sink sends events, journal_running saying whether egraph can reach a running journal.
[[nodiscard]] Targets targets(Sink sink, bool journal_running);

// Whether systemd runs the system (sd_booted's test) and egraph was built with the journal.
[[nodiscard]] bool journal_running(const std::filesystem::path& root = "/");

// ${EPREFIX}/var/log/egraph.log.
[[nodiscard]] std::filesystem::path default_file(const std::filesystem::path& eprefix);

// A new run's id: 16 hex digits, random.
[[nodiscard]] std::string new_run();

// Writes events where targets say, saying once on notes why one cannot be written, as the run
// goes on without it.
class Log {
  public:
    Log(Targets targets, std::filesystem::path file, std::ostream& notes EGRAPH_KEPT_BY_THIS);

    void write(const Event& event);

  private:
    Targets targets_;
    std::filesystem::path file_;
    std::reference_wrapper<std::ostream> notes_;
    bool told_journal_ = false;
    bool told_file_ = false;
};

} // namespace egraph::log
