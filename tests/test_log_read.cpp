#include "log_read.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using egraph::log::Event;
using egraph::log::RunSummary;

namespace {

const std::chrono::time_zone& utc() {
    return *std::chrono::locate_zone("UTC");
}

Event event(std::string run, std::string kind, double time, std::vector<egraph::log::Field> fields,
            std::string message = "m") {
    return {.time = time,
            .run = std::move(run),
            .kind = std::move(kind),
            .message = std::move(message),
            .priority = 6,
            .fields = std::move(fields)};
}

} // namespace

TEST_CASE("the file's lines read back as the events written") {
    const auto written = event("run1", "merged", 12.5,
                               {{.name = "cpv", .value = std::string{"app-misc/a-1"}},
                                {.name = "step", .value = std::int64_t{2}},
                                {.name = "seconds", .value = 1.5}});
    const auto text = egraph::log::file_line(written) + "\nnot json\n\n[1]\n" +
                      egraph::log::file_line(event("run1", "end", 13, {})) + '\n';
    const auto events = egraph::log::file_events(text);
    REQUIRE(events.size() == 2);
    CHECK(events.at(0).time == 12.5);
    CHECK(events.at(0).run == "run1");
    CHECK(events.at(0).kind == "merged");
    CHECK(events.at(0).message == "m");
    CHECK(events.at(0).priority == 6);
    CHECK(std::is_permutation(events.at(0).fields.begin(), events.at(0).fields.end(),
                              written.fields.begin(), written.fields.end()));
    CHECK(events.at(1).kind == "end");
}

TEST_CASE("the journal's entries read back as egraph's events, their fields as strings") {
    const auto text =
        R"({"__REALTIME_TIMESTAMP":"1700000000500000","MESSAGE":"app-misc/a-1: merged","PRIORITY":"6",)"
        R"("SYSLOG_IDENTIFIER":"egraph","EGRAPH_RUN":"run1","EGRAPH_EVENT":"merged",)"
        R"("EGRAPH_CPV":"app-misc/a-1","EGRAPH_SECONDS":"2.5","_PID":"1"})"
        "\n"
        R"({"__REALTIME_TIMESTAMP":"1","MESSAGE":"x","SYSLOG_IDENTIFIER":"egraph-test"})"
        "\n"
        R"({"__REALTIME_TIMESTAMP":"2","MESSAGE":[1,2],"PRIORITY":"3","SYSLOG_IDENTIFIER":"egraph",)"
        R"("EGRAPH_RUN":"run1","EGRAPH_EVENT":"end","EGRAPH_STATUS":"ok"})"
        "\n";
    const auto events = egraph::log::journal_events(text);
    REQUIRE(events.size() == 2);
    CHECK(events.at(0).time == 1700000000.5);
    CHECK(events.at(0).run == "run1");
    CHECK(events.at(0).kind == "merged");
    CHECK(events.at(0).message == "app-misc/a-1: merged");
    CHECK(events.at(0).priority == 6);
    const std::vector<egraph::log::Field> strings{
        {.name = "cpv", .value = std::string{"app-misc/a-1"}},
        {.name = "seconds", .value = std::string{"2.5"}}};
    CHECK(std::is_permutation(events.at(0).fields.begin(), events.at(0).fields.end(),
                              strings.begin(), strings.end()));
    // A message the journal holds as bytes is left out.
    CHECK(events.at(1).message.empty());
    CHECK(events.at(1).priority == 3);
    CHECK(egraph::log::journal_command() == std::vector<std::string>{"journalctl", "--no-pager",
                                                                     "--output=json",
                                                                     "--identifier=egraph"});
}

TEST_CASE("runs are summed up from their first and last events, typed or from the journal") {
    const std::vector<Event> events{
        event("run1", "run", 1700000000,
              {{.name = "command", .value = std::string{"exec"}},
               {.name = "targets", .value = std::string{"app-misc/a app-misc/b"}}}),
        event("run2", "run", 1700000100,
              {{.name = "command", .value = std::string{"exec"}},
               {.name = "targets", .value = std::string{"@world"}}}),
        event("run1", "merged", 1700000050, {}),
        event("run1", "end", 1700000125,
              {{.name = "status", .value = std::string{"ok"}},
               {.name = "merged", .value = std::int64_t{2}},
               {.name = "uninstalled", .value = std::int64_t{0}},
               {.name = "failed", .value = std::int64_t{1}},
               {.name = "skipped", .value = std::int64_t{0}},
               {.name = "seconds", .value = 125.0}}),
        event("run3", "run", 1700000200, {{.name = "command", .value = std::string{"exec"}}}),
        event("run3", "end", 1700000212,
              {{.name = "status", .value = std::string{"failed"}},
               {.name = "merged", .value = std::string{"3"}},
               {.name = "seconds", .value = std::string{"12.5"}}})};
    const auto runs = egraph::log::summarize(events);
    REQUIRE(runs.size() == 3);
    CHECK(runs.at(0).run == "run1");
    CHECK(runs.at(0).started == 1700000000);
    CHECK(runs.at(0).command == "exec");
    CHECK(runs.at(0).targets == "app-misc/a app-misc/b");
    CHECK(runs.at(0).status == "done");
    CHECK(runs.at(0).merged == 2);
    CHECK(runs.at(0).failed == 1);
    CHECK(runs.at(0).seconds == 125);
    CHECK(runs.at(1).status == "unfinished");
    CHECK_FALSE(runs.at(1).seconds);
    CHECK(runs.at(2).status == "failed");
    CHECK(runs.at(2).merged == 3);
    CHECK(runs.at(2).seconds == 12.5);

    CHECK(egraph::log::summary_line(runs.at(0), utc()) ==
          "2023-11-14 22:13:20  run1  exec app-misc/a app-misc/b  done: 2 merged, 0 "
          "uninstalled, 1 failed, 0 skipped in 2 min 5 s");
    CHECK(egraph::log::summary_line(runs.at(1), utc()) ==
          "2023-11-14 22:15:00  run2  exec @world  unfinished");
    CHECK(egraph::log::summary_line(runs.at(2), utc()) ==
          "2023-11-14 22:16:40  run3  exec  failed: 3 merged, 0 uninstalled, 0 failed, 0 "
          "skipped in 12.5 s");
    CHECK(egraph::log::summary_fields(runs.at(0)) ==
          "run1\t1700000000\texec\tdone\t2\t0\t1\t0\t125.0\tapp-misc/a app-misc/b");
    CHECK(egraph::log::summary_fields(runs.at(1)) ==
          "run2\t1700000100\texec\tunfinished\t0\t0\t0\t0\t\t@world");
}

TEST_CASE("a run is found by its id or the start of it") {
    const std::vector<RunSummary> runs{{.run = "3f2a01"}, {.run = "3f2b02"}, {.run = "9c0003"}};
    CHECK(egraph::log::find_run(runs, "9c0003") == "9c0003");
    CHECK(egraph::log::find_run(runs, "9") == "9c0003");
    CHECK(egraph::log::find_run(runs, "3f2b") == "3f2b02");
    CHECK_FALSE(egraph::log::find_run(runs, "3f2"));
    CHECK_FALSE(egraph::log::find_run(runs, "aa"));
    CHECK_FALSE(egraph::log::find_run(runs, ""));
}

TEST_CASE("an event reads as its time and message") {
    CHECK(
        egraph::log::event_line(event("run1", "merged", 1700000000.75, {}, "app-misc/a-1: merged"),
                                utc()) == "2023-11-14 22:13:20  app-misc/a-1: merged");
}
