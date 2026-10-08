#include "status.hpp"

#include "evaluated.hpp"
#include "helpers.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
    auto file = notice_file();
    file.notices.push_back({.kind = egraph::NoticeKind::news,
                            .key = "news:gentoo/2026-09-01-x",
                            .title = "X happened",
                            .file = "/repo/metadata/news/2026-09-01-x/2026-09-01-x.en.txt",
                            .fingerprint = "2026-09-01-x"});
    const auto read = egraph::parse_notice_file(egraph::notice_file_json(file));
    REQUIRE(read);
    CHECK(*read == file);
}

TEST_CASE("a notices file from before notices named their packages and files is read without") {
    auto text = egraph::notice_file_json(notice_file());
    for (const std::string field : {R"("file":"",)", R"("packages":["dev-libs/foo-2"],)"}) {
        REQUIRE(text.contains(field));
        text.replace(text.find(field), field.size(), "");
    }
    const auto read = egraph::parse_notice_file(text);
    REQUIRE(read);
    CHECK(read->notices.front().packages.empty());
    CHECK(read->notices.front().file.empty());
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
    CHECK(egraph::notices_summary(std::vector{notice(NoticeKind::plan)}) ==
          "1 notice: the plan changed by an edit");
    CHECK(
        egraph::notices_summary(std::vector{notice(NoticeKind::check), notice(NoticeKind::plan)}) ==
        "2 notices: the plan changed by an edit, the configuration check");
    CHECK(egraph::notices_summary(std::vector{notice(NoticeKind::news)}) ==
          "1 notice: 1 news item");
}

namespace {

egraph::Input input(std::string path, std::uint64_t mtime = 1) {
    return {.path = std::move(path), .kind = egraph::InputKind::file, .mtime_ns = mtime, .size = 1};
}

using Inputs = std::vector<egraph::Input>;
using Layers = std::vector<std::span<const egraph::Input>>;

} // namespace

TEST_CASE("the configuration's inputs are those under its directory, each once") {
    const Inputs installed{input("/etc/portage"), input("/etc/portage/make.conf"),
                           input("/var/db/pkg")};
    const Inputs evaluated{input("/etc/portage/package.use/foo"), input("/etc/portage/make.conf"),
                           input("/etc/portage-old/make.conf"), input("/var/db/repos/gentoo")};
    CHECK(egraph::config_inputs(Layers{installed, evaluated}, "/etc/portage/") ==
          Inputs{input("/etc/portage"), input("/etc/portage/make.conf"),
                 input("/etc/portage/package.use/foo")});
}

TEST_CASE("the digest of the other inputs changes with them alone") {
    const Inputs layer{input("/etc/portage/make.conf"), input("/var/db/pkg"),
                       input("/var/db/repos/gentoo")};
    const auto digest = egraph::other_inputs_digest(Layers{layer}, "/etc/portage");
    CHECK(digest.size() == 16);
    const Inputs reordered{input("/var/db/repos/gentoo"), input("/var/db/pkg"),
                           input("/var/db/pkg")};
    CHECK(egraph::other_inputs_digest(Layers{reordered, layer}, "/etc/portage") == digest);
    const Inputs edited{input("/etc/portage/make.conf", 2), input("/var/db/pkg"),
                        input("/var/db/repos/gentoo")};
    CHECK(egraph::other_inputs_digest(Layers{edited}, "/etc/portage") == digest);
    const Inputs merged{input("/etc/portage/make.conf"), input("/var/db/pkg", 2),
                        input("/var/db/repos/gentoo")};
    CHECK(egraph::other_inputs_digest(Layers{merged}, "/etc/portage") != digest);
}

namespace {

egraph::Status planned(Inputs config, std::vector<std::string> lines,
                       egraph::PlanCounts counts = {}) {
    return {.written = at(2026y / oct / 7, 12h),
            .stores = {},
            .counts = counts,
            .repositories = {},
            .lines = std::move(lines),
            .config = std::move(config),
            .others = "0123456789abcdef"};
}

} // namespace

TEST_CASE("a configuration edit alone changing the plan is a plan change") {
    const auto before = planned(
        {input("/etc/portage/package.use/foo"), input("/etc/portage/package.use/old")},
        {"dev-libs/a-1\tupgrade\tdev-libs/a-2\tgentoo", "dev-libs/b-1\theld\tdev-libs/b-2\tgentoo",
         "dev-libs/b-1\tholder\tdev-libs/z-1", "dev-libs/c-1\trebuild\tdev-libs/c-1\tgentoo\tx"},
        {.upgrades = 1, .rebuilds = 1, .held = 1});
    auto after =
        planned({input("/etc/portage/package.mask"), input("/etc/portage/package.use/foo", 2)},
                {"dev-libs/a-1\tupgrade\tdev-libs/a-2\tgentoo",
                 "dev-libs/c-1\trebuild\tdev-libs/c-1\tgentoo\tx",
                 "dev-libs/d-1\trebuild\tdev-libs/d-1\tgentoo\ty", "dev-libs/d-1\tnodeps"},
                {.upgrades = 1, .rebuilds = 2});
    after.written += 1h;
    const auto change = egraph::plan_change(before, after);
    REQUIRE(change);
    CHECK(change->files == std::vector<std::string>{"/etc/portage/package.mask",
                                                    "/etc/portage/package.use/foo",
                                                    "/etc/portage/package.use/old"});
    CHECK(change->before == before.counts);
    CHECK(change->gained ==
          std::vector<std::string>{"dev-libs/d-1\trebuild\tdev-libs/d-1\tgentoo\ty"});
    CHECK(change->lost == std::vector<std::string>{"dev-libs/b-1\theld\tdev-libs/b-2\tgentoo"});

    SECTION("not when something else changed too") {
        after.others = "fedcba9876543210";
        CHECK_FALSE(egraph::plan_change(before, after));
    }
    SECTION("not when the configuration did not change") {
        after.config = before.config;
        CHECK_FALSE(egraph::plan_change(before, after));
    }
    SECTION("not when the plan did not change") {
        after.lines = before.lines;
        after.counts = before.counts;
        CHECK_FALSE(egraph::plan_change(before, after));
    }
    SECTION("not after a status that kept no inputs") {
        auto older = before;
        older.config.clear();
        older.others.clear();
        CHECK_FALSE(egraph::plan_change(older, after));
    }
}

TEST_CASE("a plan change is a notice of what the edit did") {
    auto status = planned({}, {}, {.upgrades = 1, .rebuilds = 3, .refused = true});
    CHECK_FALSE(egraph::plan_notice(status));
    status.change = egraph::PlanChange{.files = {"/etc/portage/package.use/foo"},
                                       .before = {.upgrades = 2, .rebuilds = 1, .held = 1},
                                       .gained = {"dev-libs/d-1\trebuild\tdev-libs/d-1\tgentoo\ty"},
                                       .lost = {"dev-libs/b-1\theld\tdev-libs/b-2\tgentoo"}};
    const auto notice = egraph::plan_notice(status);
    REQUIRE(notice);
    CHECK(notice->kind == egraph::NoticeKind::plan);
    CHECK(notice->key == "plan");
    CHECK(notice->title == "Configuration edit: -1 upgrade, +2 rebuilds, -1 held, now refused");
    CHECK(notice->detail == std::vector<std::string>{"edited /etc/portage/package.use/foo",
                                                     "+ dev-libs/d-1 rebuild dev-libs/d-1 gentoo y",
                                                     "- dev-libs/b-1 held dev-libs/b-2 gentoo"});
    CHECK(notice->fingerprint == std::format("{}", status.written.time_since_epoch().count()));
    CHECK(notice->since == status.written);

    status.counts = status.change->before;
    CHECK(egraph::plan_notice(status)->title == "Configuration edit: the plan changed");
}

TEST_CASE("a status keeps its configuration's inputs and its plan change through JSON") {
    auto status = planned({input("/etc/portage/make.conf"),
                           {.path = "/etc/portage/package.use",
                            .kind = egraph::InputKind::directory,
                            .mtime_ns = 5,
                            .size = 0}},
                          {"dev-libs/a-1\tupgrade\tdev-libs/a-2\tgentoo"});
    status.change = egraph::PlanChange{.files = {"/etc/portage/make.conf"},
                                       .before = {.rebuilds = 2, .refused = true},
                                       .gained = {"x"},
                                       .lost = {"y", "z"}};
    CHECK(egraph::parse_status(egraph::status_json(status)) == status);
    status.change.reset();
    CHECK(egraph::parse_status(egraph::status_json(status)) == status);
}
