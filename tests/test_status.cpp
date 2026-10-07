#include "status.hpp"

#include "evaluated.hpp"
#include "helpers.hpp"
#include "store_writer.hpp"

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
        .stores = {.installed = {.path = "/var/cache/egraph/installed.egraph", .built = 1},
                   .evaluated = {.path = "/var/cache/egraph/installed.evaluated.egraph",
                                 .built = 2},
                   .repository = {.path = "/var/cache/egraph/installed.repository.egraph",
                                  .built = 18'000'000'000'000'000'000U}},
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
    const auto stores = [](std::uint64_t installed, std::uint64_t evaluated,
                           std::uint64_t repository) {
        return StatusStores{.installed = {.path = "i", .built = installed},
                            .evaluated = {.path = "e", .built = evaluated},
                            .repository = {.path = "r", .built = repository}};
    };
    const auto before = stores(1, 1, 1);
    const auto merged = stores(2, 2, 1);
    const auto synced = stores(1, 2, 2);
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

TEST_CASE("a store's build time is read from its header and meta alone") {
    const TempDir dir;
    const auto installed = dir.path() / "installed.egraph";
    // Packages that do not decode.
    egraph::test::write_bytes(installed,
                              egraph::test::with_section(4, egraph::test::Bytes{}.varint(9)));
    REQUIRE(!egraph::load(installed));
    CHECK(egraph::store_build_time(installed) == 42);
    const auto evaluated = dir.path() / "installed.evaluated.egraph";
    egraph::test::write_bytes(evaluated,
                              egraph::test::assemble_evaluated(egraph::test::evaluated_sections()));
    CHECK(egraph::evaluated_build_time(evaluated) == 43);
    CHECK(egraph::store_build_time(evaluated).error().message ==
          evaluated.string() + ": not an egraph store");
    CHECK(egraph::store_build_time(dir.path() / "none.egraph")
              .error()
              .message.starts_with((dir.path() / "none.egraph").string() + ": "));
    egraph::test::write_bytes(installed,
                              egraph::test::assemble(egraph::test::sample_sections(), 1));
    const auto old = egraph::store_build_time(installed);
    REQUIRE(old.error().mismatch);
    CHECK(old.error().mismatch->found == 1);
    egraph::test::write_bytes(installed, egraph::test::assemble({}, egraph::store_format_version));
    CHECK(!egraph::store_build_time(installed));
}

TEST_CASE("a status is current while its stores record the build times it does") {
    const TempDir dir;
    const auto installed = dir.path() / "installed.egraph";
    const auto evaluated = dir.path() / "installed.evaluated.egraph";
    egraph::test::write_bytes(installed, egraph::test::assemble(egraph::test::sample_sections()));
    egraph::test::write_bytes(evaluated,
                              egraph::test::assemble_evaluated(egraph::test::evaluated_sections()));
    // No repository index decodes without one, so the evaluated store stands in for its path.
    StatusStores stores{.installed = {.path = installed.string(), .built = 42},
                        .evaluated = {.path = evaluated.string(), .built = 43},
                        .repository = {.path = (dir.path() / "none").string(), .built = 1}};
    CHECK(!egraph::stores_unchanged(stores));
}

TEST_CASE("ages, rounded down") {
    const auto now = at(2026y / oct / 6, 12h);
    CHECK(egraph::age_text(now - 59s, now) == "just now");
    CHECK(egraph::age_text(now - 1min, now) == "1 minute ago");
    CHECK(egraph::age_text(now - 119min, now) == "1 hour ago");
    CHECK(egraph::age_text(now - 5h, now) == "5 hours ago");
    CHECK(egraph::age_text(now - 47h, now) == "1 day ago");
    CHECK(egraph::age_text(now - 72h, now) == "3 days ago");
    // A clock set back.
    CHECK(egraph::age_text(now + 1h, now) == "just now");
}

TEST_CASE("a status summed up for a terminal") {
    const auto now = at(2026y / oct / 6, 12h);
    egraph::Status status{
        .written = now - 5min,
        .stores = {},
        .counts = {.upgrades = 143, .rebuilds = 4, .held = 2},
        .repositories = {{.name = "gentoo", .synced = now - 72h}, {.name = "local"}},
        .lines = {}};
    CHECK(egraph::status_summary(status, true, now) ==
          std::vector<std::string>{"143 upgrades, 4 rebuilds, 2 held", "gentoo synced 3 days ago",
                                   "planned 5 minutes ago"});
    status.counts = {.downgrades = 1, .added = 1, .uninstalls = 2, .masked = 1, .refused = true};
    status.repositories.clear();
    CHECK(egraph::status_summary(status, false, now) ==
          std::vector<std::string>{
              "1 downgrade, 1 new, 2 uninstalls, 1 masked; emerge would refuse the plan",
              "planned 5 minutes ago, before the stores last changed"});
    status.counts = {};
    CHECK(egraph::status_summary(status, true, now).front() == "no updates");
}

TEST_CASE("a status as lines for a status bar") {
    const egraph::Status status{
        .written = at(2026y / oct / 6, 12h),
        .stores = {},
        .counts = {.upgrades = 3, .held = 1, .refused = true},
        .repositories = {{.name = "gentoo", .synced = at(2026y / oct / 5)}, {.name = "local"}},
        .lines = {}};
    CHECK(egraph::status_lines(status, false) ==
          std::vector<std::string>{"upgrades\t3", "downgrades\t0", "rebuilds\t0", "new\t0",
                                   "held\t1", "uninstalls\t0", "masked\t0", "refused\tyes",
                                   "current\tno", "written\t1791288000",
                                   "synced\tgentoo\t1791158400", "synced\tlocal\t"});
}

TEST_CASE("a status as JSON says whether it is current when asked") {
    const egraph::Status status;
    CHECK(egraph::status_json(status).find("current") == std::string::npos);
    CHECK(egraph::status_json(status, true).find("\"current\":true") != std::string::npos);
    CHECK(egraph::parse_status(egraph::status_json(status, false)) == status);
}

namespace {

egraph::NoticeFile notice_file() {
    using namespace std::chrono;
    egraph::NoticeFile file;
    file.written = sys_days{2026y / October / 9};
    file.stores = {
        .installed = {.path = "/var/cache/egraph/installed.egraph", .built = 1},
        .evaluated = {.path = "/var/cache/egraph/installed.evaluated.egraph", .built = 2},
        .repository = {.path = "/var/cache/egraph/installed.repository.egraph", .built = 3}};
    file.repositories = {
        {.name = "gentoo", .synced = egraph::Seconds{sys_days{2026y / October / 1}}},
        {.name = "local", .synced = std::nullopt}};
    file.notices = {{.kind = egraph::NoticeKind::glsa,
                     .key = "glsa:202601-01",
                     .title = "GLSA 202601-01: foo",
                     .detail = {"dev-libs/foo-2"},
                     .packages = {"dev-libs/foo-2"},
                     .fingerprint = "2 dev-libs/foo-2",
                     .since = sys_days{2026y / October / 2}},
                    {.kind = egraph::NoticeKind::stale,
                     .key = "stale:gentoo",
                     .title = "gentoo synced 7 days ago",
                     .detail = {},
                     .fingerprint = "1790812800",
                     .since = sys_days{2026y / October / 8}}};
    return file;
}

} // namespace

TEST_CASE("a notices file round-trips through JSON") {
    const auto file = notice_file();
    const auto read = egraph::parse_notice_file(egraph::notice_file_json(file));
    REQUIRE(read);
    CHECK(*read == file);
}

TEST_CASE("a notices file from before notices named their packages is read without them") {
    auto text = egraph::notice_file_json(notice_file());
    const std::string field = R"("packages":["dev-libs/foo-2"],)";
    REQUIRE(text.contains(field));
    text.replace(text.find(field), field.size(), "");
    const auto read = egraph::parse_notice_file(text);
    REQUIRE(read);
    CHECK(read->notices.front().packages.empty());
}

TEST_CASE("a notices file of another format or none at all is refused") {
    CHECK(egraph::parse_notice_file("[]").error() == "not a notices file");
    CHECK(egraph::parse_notice_file(R"({"format": 1, "written": 1})").error() ==
          "not a notices file");
    CHECK(egraph::parse_notice_file(R"({"format": 9})").error() ==
          "format 9, from another egraph version");
    auto text = egraph::notice_file_json(notice_file());
    text.replace(text.find("\"glsa\""), 6, "\"what\"");
    CHECK(egraph::parse_notice_file(text).error() == "not a notices file");
}

TEST_CASE("stale repositories are found again as time passes") {
    using namespace std::chrono;
    const auto file = notice_file();
    const auto at = [&](sys_days day, int stale_days) {
        std::vector<std::string> found;
        for (const auto& notice : egraph::current_notices(file, day, stale_days)) {
            found.push_back(notice.key + " " + notice.title);
        }
        return found;
    };
    CHECK(at(2026y / October / 5, 7) ==
          std::vector<std::string>{"glsa:202601-01 GLSA 202601-01: foo"});
    CHECK(at(2026y / October / 12, 7) ==
          std::vector<std::string>{"glsa:202601-01 GLSA 202601-01: foo",
                                   "stale:gentoo gentoo synced 11 days ago"});
    CHECK(at(2026y / October / 12, 0).size() == 1);
    // A stale notice keeps when it was first noticed; one noticed now dates from when it went
    // stale.
    const auto later = egraph::current_notices(file, sys_days{2026y / October / 12}, 7);
    CHECK(later.back().since == egraph::Seconds{sys_days{2026y / October / 8}});
    auto unseen = file;
    unseen.notices.pop_back();
    const auto found = egraph::current_notices(unseen, sys_days{2026y / October / 12}, 7);
    CHECK(found.back().since == egraph::Seconds{sys_days{2026y / October / 8}});
}

TEST_CASE("notices summed up in a line") {
    CHECK_FALSE(egraph::notices_summary({}));
    const auto notice = [](egraph::NoticeKind kind) {
        return egraph::Notice{
            .kind = kind, .key = "", .title = "", .detail = {}, .fingerprint = "", .since = {}};
    };
    using egraph::NoticeKind;
    const std::vector<egraph::Notice> notices{
        notice(NoticeKind::glsa),      notice(NoticeKind::missing), notice(NoticeKind::missing),
        notice(NoticeKind::preserved), notice(NoticeKind::masked),  notice(NoticeKind::stale),
        notice(NoticeKind::config),    notice(NoticeKind::news),    notice(NoticeKind::news)};
    CHECK(egraph::notices_summary(notices) ==
          "9 notices: 1 GLSA, 2 packages missing libraries, preserved libraries, 1 masked "
          "package, 1 stale repository, configuration updates, 2 news items");
    CHECK(egraph::notices_summary(std::vector{notice(NoticeKind::news)}) ==
          "1 notice: 1 news item");
}
