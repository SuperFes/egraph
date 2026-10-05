#include "log.hpp"

#include "os.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <random>
#include <type_traits>

namespace egraph::log {

using Json = nlohmann::json;

std::string file_line(const Event& event) {
    Json json{{"time", event.time},
              {"run", event.run},
              {"event", event.kind},
              {"message", event.message},
              {"priority", event.priority}};
    for (const auto& field : event.fields) {
        std::visit([&](const auto& value) { json.emplace(field.name, value); }, field.value);
    }
    return json.dump();
}

std::vector<std::string> journal_fields(const Event& event) {
    std::vector<std::string> found{
        "MESSAGE=" + event.message, std::format("PRIORITY={}", event.priority),
        "SYSLOG_IDENTIFIER=egraph", "EGRAPH_RUN=" + event.run, "EGRAPH_EVENT=" + event.kind};
    for (const auto& field : event.fields) {
        auto name = field.name;
        std::ranges::transform(name, name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto value = std::visit(
            [](const auto& each) -> std::string {
                if constexpr (std::is_same_v<std::decay_t<decltype(each)>, std::string>) {
                    return each;
                } else {
                    return Json(each).dump();
                }
            },
            field.value);
        found.push_back(std::format("EGRAPH_{}={}", name, value));
    }
    return found;
}

std::string duration(double seconds) {
    if (seconds < 60) {
        return std::format("{:.1f} s", seconds);
    }
    const auto whole = static_cast<std::int64_t>(seconds);
    if (whole < 3600) {
        return std::format("{} min {} s", whole / 60, whole % 60);
    }
    return std::format("{} h {} min", whole / 3600, whole % 3600 / 60);
}

Targets targets(Sink sink, bool journal_running) {
    switch (sink) {
    case Sink::automatic:
        return {.journal = journal_running, .file = !journal_running};
    case Sink::journal:
        return {.journal = true, .file = false};
    case Sink::file:
        return {.journal = false, .file = true};
    case Sink::both:
        return {.journal = true, .file = true};
    case Sink::none:
        return {};
    }
    return {};
}

bool journal_running(const std::filesystem::path& root) {
    std::error_code ignored;
    return os::journal_built() &&
           std::filesystem::is_directory(root / "run/systemd/system", ignored);
}

std::filesystem::path default_file(const std::filesystem::path& eprefix) {
    return std::filesystem::path{"/"} / eprefix.relative_path() / "var/log/egraph.log";
}

std::string new_run() {
    std::random_device random;
    return std::format("{:08x}{:08x}", random(), random());
}

Log::Log(Targets targets, std::filesystem::path file, std::ostream& notes)
    : targets_{targets}, file_{std::move(file)}, notes_{notes} {}

void Log::write(const Event& event) {
    if (targets_.journal) {
        if (const auto sent = os::journal_send(journal_fields(event)); !sent && !told_journal_) {
            notes_.get() << std::format("egraph: cannot log to the journal: {}\n",
                                        sent.error().message());
            told_journal_ = true;
        }
    }
    if (targets_.file) {
        if (const auto put = os::append_locked(file_, file_line(event) + '\n');
            !put && !told_file_) {
            notes_.get() << std::format("egraph: cannot log to {}: {}\n", file_.string(),
                                        put.error().message());
            told_file_ = true;
        }
    }
}

} // namespace egraph::log
