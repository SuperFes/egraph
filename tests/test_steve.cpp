#include "steve.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::steve::Setting;

TEST_CASE("stevie prints every setting, one per line, in the order asked") {
    CHECK(egraph::steve::get_arguments() ==
          std::vector<std::string>{"stevie", "--get-tokens", "--get-jobs", "--get-min-jobs",
                                   "--get-load-average", "--get-load-recheck-timeout",
                                   "--get-min-memory-avail", "--get-per-process-limit"});
    // As steve 1.5 answers for --jobs=12 --load-average=12 --min-memory-avail=4096.
    const auto settings = egraph::steve::parse_get("5\n12\n1\n12\n0.5\n4096\n0\n");
    REQUIRE(settings.has_value());
    CHECK(settings->tokens == 5);
    CHECK(settings->jobs == 12);
    CHECK(settings->min_jobs == 1);
    CHECK(settings->load_average == 12.0);
    CHECK(settings->recheck_timeout == 0.5);
    CHECK(settings->min_memory == 4096);
    // No limit is 0 or below.
    CHECK_FALSE(settings->per_process.has_value());

    const auto unlimited = egraph::steve::parse_get("12\n12\n1\n-1\n0.5\n-1\n0\n");
    REQUIRE(unlimited.has_value());
    CHECK_FALSE(unlimited->load_average.has_value());
    CHECK_FALSE(unlimited->min_memory.has_value());

    CHECK_FALSE(egraph::steve::parse_get("unable to open /dev/steve: Permission denied\n"));
    CHECK_FALSE(egraph::steve::parse_get("5\n12\n"));
    CHECK_FALSE(egraph::steve::parse_get("5\n12\n1\n12\n0.5\n4096\nx\n"));
}

TEST_CASE("stevie changes one setting at a time") {
    CHECK(egraph::steve::set_arguments(Setting::jobs, 8) ==
          std::vector<std::string>{"stevie", "--set-jobs", "8"});
    CHECK(egraph::steve::set_arguments(Setting::load_average, 12.5) ==
          std::vector<std::string>{"stevie", "--set-load-average", "12.5"});
    CHECK(egraph::steve::set_arguments(Setting::recheck_timeout, 0.25) ==
          std::vector<std::string>{"stevie", "--set-load-recheck-timeout", "0.25"});
    CHECK(egraph::steve::set_arguments(Setting::min_memory, 4352) ==
          std::vector<std::string>{"stevie", "--set-min-memory-avail", "4352"});
    CHECK(egraph::steve::set_arguments(Setting::min_jobs, 2) ==
          std::vector<std::string>{"stevie", "--set-min-jobs", "2"});
    CHECK(egraph::steve::set_arguments(Setting::per_process, 0) ==
          std::vector<std::string>{"stevie", "--set-per-process-limit", "0"});
}

TEST_CASE("steve's command line gives its settings in every getopt spelling") {
    const std::vector<std::string> service{"/usr/bin/steve", "--jobs=12", "--load-average=12",
                                           "--min-memory-avail=4096"};
    const auto settings = egraph::steve::parse_command_line(service);
    REQUIRE(settings.has_value());
    CHECK(settings->jobs == 12);
    CHECK(settings->load_average == 12.0);
    CHECK(settings->min_memory == 4096);
    // Defaults where the command line is silent; tokens only stevie knows.
    CHECK(settings->min_jobs == 1);
    CHECK(settings->recheck_timeout == 0.5);
    CHECK_FALSE(settings->per_process.has_value());
    CHECK_FALSE(settings->tokens.has_value());

    const std::vector<std::string> spelled{"steve", "-vj", "6",  "-l8.5", "--user",     "steve",
                                           "-p",    "3",   "-r", "1",     "--min-jobs", "2"};
    const auto other = egraph::steve::parse_command_line(spelled);
    REQUIRE(other.has_value());
    CHECK(other->jobs == 6);
    CHECK(other->load_average == 8.5);
    CHECK(other->per_process == 3);
    CHECK(other->recheck_timeout == 1.0);
    CHECK(other->min_jobs == 2);
    CHECK_FALSE(other->min_memory.has_value());

    // Unset jobs means one per CPU, which only steve resolves.
    const std::vector<std::string> bare{"steve"};
    CHECK_FALSE(egraph::steve::parse_command_line(bare)->jobs.has_value());
    const std::vector<std::string> stevie{"stevie", "make"};
    CHECK_FALSE(egraph::steve::parse_command_line(stevie).has_value());
    const std::vector<std::string> broken{"steve", "--jobs"};
    CHECK_FALSE(egraph::steve::parse_command_line(broken).has_value());
}

TEST_CASE("the running steve is found by name") {
    const egraph::test::TempDir dir;
    const auto& proc = dir.path();
    for (const auto* pid : {"1", "2603", "3000", "self"}) {
        std::filesystem::create_directories(proc / pid);
    }
    egraph::test::write_text(proc / "1" / "comm", "init\n");
    egraph::test::write_text(proc / "1" / "cmdline", std::string{"init\0", 5});
    egraph::test::write_text(proc / "3000" / "comm", "stevie\n");
    egraph::test::write_text(proc / "3000" / "cmdline", std::string{"stevie\0make\0", 12});
    CHECK_FALSE(egraph::steve::find_command_line(proc).has_value());

    egraph::test::write_text(proc / "2603" / "comm", "steve\n");
    egraph::test::write_text(proc / "2603" / "cmdline",
                             std::string{"/usr/bin/steve\0--jobs=12\0", 25});
    CHECK(egraph::steve::find_command_line(proc) ==
          std::vector<std::string>{"/usr/bin/steve", "--jobs=12"});
}

TEST_CASE("settings step within what steve accepts") {
    const egraph::steve::Settings settings{
        .jobs = 12, .min_jobs = 1, .load_average = 12, .recheck_timeout = 0.5, .min_memory = 4096};
    CHECK(egraph::steve::step(Setting::jobs, settings, 1, 16) == 13.0);
    CHECK(egraph::steve::step(Setting::jobs, {.jobs = 1}, -1, 16) == std::nullopt);
    CHECK(egraph::steve::step(Setting::min_jobs, settings, 1, 16) == 2.0);
    // min-jobs can go no higher than jobs.
    CHECK(egraph::steve::step(Setting::min_jobs, {.jobs = 2, .min_jobs = 2}, 1, 16) ==
          std::nullopt);
    CHECK(egraph::steve::step(Setting::load_average, settings, -1, 16) == 11.0);
    CHECK(egraph::steve::step(Setting::load_average, {.load_average = 1}, -1, 16) == std::nullopt);
    // With no limit, the first step up is one per CPU.
    CHECK(egraph::steve::step(Setting::load_average, {}, 1, 16) == 16.0);
    CHECK(egraph::steve::step(Setting::load_average, {}, -1, 16) == std::nullopt);
    CHECK(egraph::steve::step(Setting::min_memory, settings, 1, 16) == 4352.0);
    CHECK(egraph::steve::step(Setting::min_memory, {.min_memory = 100}, -1, 16) == 0.0);
    CHECK(egraph::steve::step(Setting::min_memory, {}, -1, 16) == std::nullopt);
    CHECK(egraph::steve::step(Setting::per_process, {}, 1, 16) == 1.0);
    CHECK(egraph::steve::step(Setting::per_process, {.per_process = 1}, -1, 16) == 0.0);
    CHECK(egraph::steve::step(Setting::recheck_timeout, settings, 1, 16) == 0.6);
    CHECK(egraph::steve::step(Setting::recheck_timeout, {.recheck_timeout = 0.1}, -1, 16) ==
          std::nullopt);
}
