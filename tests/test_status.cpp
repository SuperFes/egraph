#include "status.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using egraph::PlanWhen;
using egraph::Seconds;
using egraph::StatusStores;
using egraph::test::TempDir;
using namespace std::chrono_literals;

namespace {

constexpr Seconds at(std::chrono::year_month_day day, std::chrono::seconds time = 0s) {
    return std::chrono::sys_days{day} + time;
}

constexpr auto oct = std::chrono::October;

egraph::Merge merge(std::optional<std::uint32_t> replaces,
                    egraph::UpdateKind kind = egraph::UpdateKind::upgrade) {
    egraph::Merge made;
    made.replaces = replaces;
    made.kind = kind;
    return made;
}

} // namespace

TEST_CASE("the plan setting takes refresh, sync or never, refresh by default") {
    CHECK(egraph::parse_settings("")->plan == PlanWhen::refresh);
    CHECK(egraph::parse_settings("plan = sync\n")->plan == PlanWhen::sync);
    CHECK(egraph::parse_settings("plan=never")->plan == PlanWhen::never);
    CHECK(egraph::parse_settings("plan = refresh")->plan == PlanWhen::refresh);
    CHECK(egraph::parse_settings("plan = always").error() ==
          "line 1: plan takes refresh, sync or never, not always");
}

TEST_CASE("a plan's counts") {
    egraph::Plan plan;
    plan.merges = {merge(0), merge(1), merge(2, egraph::UpdateKind::downgrade),
                   merge(3, egraph::UpdateKind::rebuild), merge(std::nullopt)};
    plan.held.resize(2);
    plan.uninstalls.resize(1);
    plan.masked = {4};
    CHECK(egraph::plan_counts(plan) == egraph::PlanCounts{.upgrades = 2,
                                                          .downgrades = 1,
                                                          .rebuilds = 1,
                                                          .added = 1,
                                                          .held = 2,
                                                          .uninstalls = 1,
                                                          .masked = 1,
                                                          .refused = false});
    plan.blocks.resize(1);
    CHECK(egraph::plan_counts(plan).refused);
}

TEST_CASE("a repository's sync time is its timestamp.chk, as emerge --info reads it") {
    CHECK(egraph::parse_sync_timestamp("Tue, 06 Oct 2026 18:46:01 +0000\n") ==
          at(2026y / oct / 6, 18h + 46min + 1s));
    CHECK(egraph::parse_sync_timestamp("Tue, 06 Oct 2026 20:46:01 +0200") ==
          at(2026y / oct / 6, 18h + 46min + 1s));
    CHECK(!egraph::parse_sync_timestamp(""));
    CHECK(!egraph::parse_sync_timestamp("yesterday"));
    const TempDir dir;
    CHECK(!egraph::repository_synced(dir.path()));
    std::filesystem::create_directories(dir.path() / "metadata");
    std::ofstream{dir.path() / "metadata/timestamp.chk"} << "Tue, 06 Oct 2026 18:46:01 +0000\n";
    CHECK(egraph::repository_synced(dir.path()) == at(2026y / oct / 6, 18h + 46min + 1s));
}

TEST_CASE("a status goes to JSON and back") {
    const egraph::Status status{
        .written = at(2026y / oct / 6, 12h),
        .stores = {.installed = 1, .evaluated = 2, .repository = 18'000'000'000'000'000'000U},
        .counts = {.upgrades = 3, .held = 1, .refused = true},
        .repositories = {{.name = "gentoo", .synced = at(2026y / oct / 5)}, {.name = "local"}},
        .lines = {"app-misc/foo-1\tupgrade\tapp-misc/foo-2\tgentoo"}};
    const auto text = egraph::status_json(status);
    CHECK(text.ends_with("}\n"));
    CHECK(text.find("\"command\":\"updates --world -D -N --held\"") != std::string::npos);
    CHECK(egraph::parse_status(text) == status);
}

TEST_CASE("a status from another format, or not one, is refused") {
    CHECK(egraph::parse_status("{\"format\":2}").error() ==
          "format 2, from another egraph version");
    CHECK(egraph::parse_status("[]").error() == "not a status file");
    CHECK(egraph::parse_status("{").error() == "not a status file");
}

TEST_CASE("the status file is beside the installed store") {
    CHECK(egraph::status_path("/var/cache/egraph/installed.egraph") ==
          "/var/cache/egraph/status.json");
}

TEST_CASE("which refreshes plan anew") {
    const StatusStores before{.installed = 1, .evaluated = 1, .repository = 1};
    const StatusStores merged{.installed = 2, .evaluated = 2, .repository = 1};
    const StatusStores synced{.installed = 1, .evaluated = 2, .repository = 2};
    CHECK(egraph::status_due(PlanWhen::refresh, std::nullopt, before));
    CHECK(egraph::status_due(PlanWhen::sync, std::nullopt, before));
    CHECK(!egraph::status_due(PlanWhen::never, std::nullopt, before));
    CHECK(!egraph::status_due(PlanWhen::refresh, before, before));
    CHECK(egraph::status_due(PlanWhen::refresh, before, merged));
    CHECK(egraph::status_due(PlanWhen::refresh, before, synced));
    CHECK(!egraph::status_due(PlanWhen::sync, before, merged));
    CHECK(egraph::status_due(PlanWhen::sync, before, synced));
    CHECK(!egraph::status_due(PlanWhen::never, before, synced));
}
