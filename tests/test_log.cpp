#include "log.hpp"

#include "helpers.hpp"
#include "os.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using egraph::log::Event;
using egraph::log::Sink;
using egraph::log::Targets;
using Json = nlohmann::json;

namespace {

Event merged() {
    return {.time = 1700000000.5,
            .run = "0123456789abcdef",
            .kind = "merged",
            .message = "app-misc/foo-1 merged in 2.5 s",
            .priority = 6,
            .fields = {{.name = "cpv", .value = std::string{"app-misc/foo-1"}},
                       {.name = "step", .value = std::int64_t{3}},
                       {.name = "seconds", .value = 2.5}}};
}

} // namespace

TEST_CASE("an event is a JSON line in the file, its fields beside what every event has") {
    CHECK(egraph::log::file_line(merged()) ==
          R"({"cpv":"app-misc/foo-1","event":"merged","message":"app-misc/foo-1 merged in 2.5 s",)"
          R"("priority":6,"run":"0123456789abcdef","seconds":2.5,"step":3,"time":1700000000.5})");
}

TEST_CASE("an event is journal fields, its own under EGRAPH_ in upper case") {
    CHECK(egraph::log::journal_fields(merged()) ==
          std::vector<std::string>{"MESSAGE=app-misc/foo-1 merged in 2.5 s", "PRIORITY=6",
                                   "SYSLOG_IDENTIFIER=egraph", "EGRAPH_RUN=0123456789abcdef",
                                   "EGRAPH_EVENT=merged", "EGRAPH_CPV=app-misc/foo-1",
                                   "EGRAPH_STEP=3", "EGRAPH_SECONDS=2.5"});
}

TEST_CASE("by default events go to the journal under systemd, else to the file") {
    CHECK(egraph::log::targets(Sink::automatic, true) == Targets{.journal = true, .file = false});
    CHECK(egraph::log::targets(Sink::automatic, false) == Targets{.journal = false, .file = true});
    CHECK(egraph::log::targets(Sink::journal, false) == Targets{.journal = true, .file = false});
    CHECK(egraph::log::targets(Sink::file, true) == Targets{.journal = false, .file = true});
    CHECK(egraph::log::targets(Sink::both, false) == Targets{.journal = true, .file = true});
    CHECK(egraph::log::targets(Sink::none, true) == Targets{});
}

TEST_CASE("systemd runs when its run directory is there, and egraph has the journal") {
    const egraph::test::TempDir dir;
    CHECK_FALSE(egraph::log::journal_running(dir.path()));
    std::filesystem::create_directories(dir.path() / "run/systemd/system");
    CHECK(egraph::log::journal_running(dir.path()) == egraph::os::journal_built());
}

TEST_CASE("the file is egraph's own, under the prefix") {
    CHECK(egraph::log::default_file("") == "/var/log/egraph.log");
    CHECK(egraph::log::default_file("/p") == "/p/var/log/egraph.log");
}

TEST_CASE("each run has its own id") {
    const auto run = egraph::log::new_run();
    CHECK(run.size() == 16);
    CHECK(run.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(egraph::log::new_run() != run);
}

TEST_CASE("the file gets a line for each event, made with its directories") {
    const egraph::test::TempDir dir;
    const auto path = dir.path() / "var/log/egraph.log";
    std::ostringstream notes;
    egraph::log::Log log{{.journal = false, .file = true}, path, notes};
    log.write(merged());
    auto second = merged();
    second.kind = "end";
    log.write(second);
    std::istringstream lines{egraph::test::read_text(path)};
    std::vector<std::string> kinds;
    for (std::string line; std::getline(lines, line);) {
        kinds.push_back(Json::parse(line).at("event").get<std::string>());
    }
    CHECK(kinds == std::vector<std::string>{"merged", "end"});
    CHECK(notes.str().empty());
    CHECK((std::filesystem::status(path).permissions() & std::filesystem::perms::others_read) !=
          std::filesystem::perms::none);
}

TEST_CASE("a file that cannot be written is said once, and the run goes on") {
    const egraph::test::TempDir dir;
    // Its directory is a file.
    egraph::test::write_text(dir.path() / "log", "");
    std::ostringstream notes;
    egraph::log::Log log{{.journal = false, .file = true}, dir.path() / "log/egraph.log", notes};
    log.write(merged());
    log.write(merged());
    CHECK(notes.str().starts_with("egraph: cannot log to "));
    CHECK(notes.str().find('\n') == notes.str().size() - 1);
}

TEST_CASE("nothing goes anywhere with no targets") {
    const egraph::test::TempDir dir;
    std::ostringstream notes;
    egraph::log::Log log{{}, dir.path() / "egraph.log", notes};
    log.write(merged());
    CHECK_FALSE(std::filesystem::exists(dir.path() / "egraph.log"));
    CHECK(notes.str().empty());
}
