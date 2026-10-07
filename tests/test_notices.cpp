#include "helpers.hpp"
#include "index_builder.hpp"
#include "notices.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>

#include <string>
#include <vector>

TEST_CASE("notices are read from egraph-build's JSON") {
    const auto notices = egraph::parse_notices(R"({
 "config": [{"file": "/etc/a.conf", "update": "/etc/._cfg0000_a.conf"}],
 "news": [{"item": "2026-09-01-x", "repo": "gentoo", "title": "X happened"}],
 "preserved": [{"path": "/usr/lib/libfoo.so.1", "package": "dev-libs/foo-2",
                "consumers": ["app-misc/bar-1"]}],
 "rebuild": ["app-misc/bar:0"]
})");
    REQUIRE(notices);
    REQUIRE(notices->config.size() == 1);
    CHECK(notices->config.front().file == "/etc/a.conf");
    CHECK(notices->config.front().update == "/etc/._cfg0000_a.conf");
    REQUIRE(notices->news.size() == 1);
    CHECK(notices->news.front().repo == "gentoo");
    CHECK(notices->news.front().item == "2026-09-01-x");
    CHECK(notices->news.front().title == "X happened");
    REQUIRE(notices->preserved);
    REQUIRE(notices->preserved->size() == 1);
    CHECK(notices->preserved->front().path == "/usr/lib/libfoo.so.1");
    CHECK(notices->preserved->front().package == "dev-libs/foo-2");
    CHECK(notices->preserved->front().consumers == std::vector<std::string>{"app-misc/bar-1"});
    CHECK(notices->rebuild == std::vector<std::string>{"app-misc/bar:0"});
}

TEST_CASE("preserved libraries egraph-build could not read are none") {
    const auto notices =
        egraph::parse_notices(R"({"config": [], "news": [], "preserved": null, "rebuild": null})");
    REQUIRE(notices);
    CHECK_FALSE(notices->preserved);
    CHECK_FALSE(notices->rebuild);
}

TEST_CASE("malformed notices are an error") {
    CHECK(!egraph::parse_notices("[]"));
    CHECK(!egraph::parse_notices("{"));
    CHECK(!egraph::parse_notices(
        R"({"config": [{"file": 1}], "news": [], "preserved": [], "rebuild": []})"));
    CHECK(!egraph::parse_notices(
        R"({"config": [], "news": [{"repo": "gentoo"}], "preserved": [], "rebuild": []})"));
    CHECK(!egraph::parse_notices(R"({"config": {}, "news": []})"));
    CHECK(!egraph::parse_notices(R"({"config": [], "news": [], "rebuild": []})"));
    CHECK(!egraph::parse_notices(
        R"({"config": [], "news": [], "preserved": [{"path": "/a"}], "rebuild": []})"));
    CHECK(!egraph::parse_notices(R"({"config": [], "news": [], "preserved": [], "rebuild": [1]})"));
    const auto empty =
        egraph::parse_notices(R"({"config": [], "news": [], "preserved": [], "rebuild": []})");
    REQUIRE(empty);
    CHECK(egraph::notice_lines(*empty).empty());
}

TEST_CASE("notice lines give configuration updates, news, preserved libraries, then GLSAs") {
    const egraph::Notices notices{
        .config = {{.file = "/etc/a.conf", .update = "/etc/._cfg0000_a.conf"},
                   {.file = "/etc/a.conf", .update = "/etc/._cfg0001_a.conf"}},
        .news = {{.repo = "gentoo", .item = "2026-09-01-x", .title = "X happened"}},
        .preserved =
            std::vector<egraph::Notices::Preserved>{
                {.path = "/usr/lib/libfoo.so.1",
                 .package = "dev-libs/foo-2",
                 .consumers = {"app-misc/bar-1", "app-misc/baz-1"}},
                {.path = "/usr/lib/libold.so.1", .package = "dev-libs/old-2", .consumers = {}}},
        .rebuild = std::vector<std::string>{"app-misc/bar:0", "app-misc/baz:0"},
        .advisories = {{.id = "202601-01",
                        .title = "foo: overflow",
                        .revision = 2,
                        .packages = {{.cpv = "dev-libs/foo-2",
                                      .fixed = {">=dev-libs/foo-2.1", ">=dev-libs/foo-3"}}}}},
        .stale = {},
        .masked = {},
        .missing = {}};
    CHECK(
        egraph::notice_lines(notices) ==
        std::vector<std::string>{
            "/etc/a.conf\tconfig\t/etc/._cfg0000_a.conf",
            "/etc/a.conf\tconfig\t/etc/._cfg0001_a.conf", "2026-09-01-x\tnews\tgentoo\tX happened",
            "/usr/lib/libfoo.so.1\tpreserved\tdev-libs/foo-2\tapp-misc/bar-1 app-misc/baz-1",
            "/usr/lib/libold.so.1\tpreserved\tdev-libs/old-2\t", "app-misc/bar:0\trebuild",
            "app-misc/baz:0\trebuild",
            "202601-01\tglsa\tfoo: overflow\tdev-libs/foo-2\t>=dev-libs/foo-2.1 >=dev-libs/foo-3"});
}

TEST_CASE("stale_days takes a number of days, 7 by default") {
    CHECK(egraph::parse_settings("")->stale_days == 7);
    CHECK(egraph::parse_settings("stale_days = 14\n")->stale_days == 14);
    CHECK(egraph::parse_settings("stale_days = 0")->stale_days == 0);
    CHECK(egraph::parse_settings("stale_days = weekly").error() ==
          "line 1: stale_days takes a number of days, not weekly");
}

TEST_CASE("a repository is stale once synced longer ago than the days allowed") {
    using namespace std::chrono;
    const egraph::test::TempDir root;
    egraph::test::IndexBuilder index({"gentoo", "overlay", "local"});
    for (const auto* name : {"gentoo", "overlay", "local"}) {
        std::filesystem::create_directories(root.path() / name / "metadata");
        index.locate(name, (root.path() / name).string());
    }
    egraph::test::write_text(root.path() / "gentoo/metadata/timestamp.chk",
                             "Thu, 01 Oct 2026 00:00:00 +0000\n");
    egraph::test::write_text(root.path() / "overlay/metadata/timestamp.chk",
                             "Tue, 06 Oct 2026 00:00:00 +0000\n");
    // local has none: a plain checkout, whose age is unknown.
    const egraph::Seconds now = sys_days{2026y / October / 9} + 1h;
    const auto stale = egraph::stale_repositories(index.index(), now, 7);
    REQUIRE(stale.size() == 1);
    CHECK(stale.front().name == "gentoo");
    CHECK(stale.front().synced == egraph::Seconds{sys_days{2026y / October / 1}});
    CHECK(egraph::stale_repositories(index.index(), now, 2).size() == 2);
    CHECK(egraph::stale_repositories(index.index(), now, 0).empty());
}

TEST_CASE("masked installed packages come with their reasons") {
    const auto system = egraph::test::make_system(
        {{.cpv = "app-misc/a-1"},
         {.cpv = "app-misc/b-1", .masked = {"package.mask", "~x86 keyword"}}},
        {});
    const auto masked = egraph::masked_installed(system.store, system.evaluated, true);
    REQUIRE(masked.size() == 1);
    CHECK(masked.front().cpv == "app-misc/b-1");
    CHECK(masked.front().reasons == std::vector<std::string>{"package.mask", "~x86 keyword"});
}

TEST_CASE("missing sonames are those no installed package provides") {
    const auto system = egraph::test::make_system(
        {{.cpv = "app-misc/a-1",
          .required = "x86_64:libfoo.so.1 x86_64:libgone.so.2 x86_32:libfoo.so.1"},
         {.cpv = "dev-libs/foo-1", .provides = "x86_64:libfoo.so.1"}},
        {});
    const auto missing = egraph::missing_sonames(system.store);
    REQUIRE(missing.size() == 2);
    CHECK(missing.at(0).cpv == "app-misc/a-1");
    CHECK(missing.at(0).category == "x86_32");
    CHECK(missing.at(0).soname == "libfoo.so.1");
    CHECK(missing.at(1).category == "x86_64");
    CHECK(missing.at(1).soname == "libgone.so.2");
}

TEST_CASE("notice lines end with stale repositories, masked packages and missing sonames") {
    using namespace std::chrono;
    egraph::Notices notices;
    notices.stale = {{.name = "gentoo", .synced = egraph::Seconds{seconds{1790000000}}}};
    notices.masked = {{.cpv = "app-misc/b-1", .reasons = {"package.mask", "~x86 keyword"}}};
    notices.missing = {{.cpv = "app-misc/a-1", .category = "x86_64", .soname = "libgone.so.2"}};
    notices.news = {{.repo = "gentoo", .item = "2026-09-01-x", .title = "X"}};
    CHECK(egraph::notice_lines(notices) ==
          std::vector<std::string>{"2026-09-01-x\tnews\tgentoo\tX", "gentoo\tstale\t1790000000",
                                   "app-misc/b-1\tmasked\tpackage.mask, ~x86 keyword",
                                   "app-misc/a-1\tmissing\tx86_64\tlibgone.so.2"});
}

TEST_CASE("a soname a preserved library provides is that library's notice") {
    egraph::Notices notices;
    notices.preserved = std::vector<egraph::Notices::Preserved>{
        {.path = "/usr/lib64/libold.so.1", .package = "dev-libs/old-2", .consumers = {}}};
    notices.missing = {{.cpv = "app-misc/a-1", .category = "x86_64", .soname = "libold.so.1"},
                       {.cpv = "app-misc/a-1", .category = "x86_64", .soname = "libgone.so.2"}};
    egraph::drop_preserved(notices);
    REQUIRE(notices.missing.size() == 1);
    CHECK(notices.missing.front().soname == "libgone.so.2");
}

namespace {

egraph::Notices every_kind() {
    using namespace std::chrono;
    egraph::Notices notices;
    notices.config = {{.file = "/etc/a.conf", .update = "/etc/._cfg0000_a.conf"},
                      {.file = "/etc/a.conf", .update = "/etc/._cfg0001_a.conf"},
                      {.file = "/etc/b.conf", .update = "/etc/._cfg0000_b.conf"}};
    notices.news = {{.repo = "gentoo", .item = "2026-09-01-x", .title = "X happened"},
                    {.repo = "gentoo", .item = "2026-09-02-y", .title = ""}};
    notices.preserved = std::vector<egraph::Notices::Preserved>{
        {.path = "/usr/lib/libfoo.so.1",
         .package = "dev-libs/foo-2",
         .consumers = {"app-misc/bar-1", "app-misc/baz-1"}}};
    notices.rebuild = std::vector<std::string>{"app-misc/bar:0"};
    notices.advisories = {{.id = "202601-01",
                           .title = "foo: overflow",
                           .revision = 2,
                           .packages = {{.cpv = "dev-libs/foo-2",
                                         .fixed = {">=dev-libs/foo-2.1", ">=dev-libs/foo-3"}}}}};
    notices.stale = {{.name = "gentoo", .synced = egraph::Seconds{sys_days{2026y / October / 1}}}};
    notices.masked = {{.cpv = "app-misc/b-1", .reasons = {"package.mask", "~x86 keyword"}}};
    notices.missing = {{.cpv = "app-misc/a-1", .category = "x86_64", .soname = "libgone.so.2"},
                       {.cpv = "app-misc/a-1", .category = "x86_64", .soname = "libold.so.3"}};
    return notices;
}

} // namespace

TEST_CASE("notices list as one, the most pressing first") {
    using namespace std::chrono;
    const egraph::Seconds now = sys_days{2026y / October / 9};
    const auto list = egraph::notice_list(every_kind(), now);
    std::vector<std::string> keys;
    for (const auto& notice : list) {
        keys.push_back(notice.key);
        CHECK(notice.since == now);
    }
    CHECK(keys == std::vector<std::string>{"glsa:202601-01", "missing:app-misc/a-1", "preserved",
                                           "masked:app-misc/b-1", "stale:gentoo", "config",
                                           "news:gentoo/2026-09-01-x", "news:gentoo/2026-09-02-y"});
    const auto& glsa = list.at(0);
    CHECK(glsa.kind == egraph::NoticeKind::glsa);
    CHECK(glsa.title == "GLSA 202601-01: foo: overflow");
    CHECK(glsa.detail == std::vector<std::string>{
                             "dev-libs/foo-2, fixed in >=dev-libs/foo-2.1 or >=dev-libs/foo-3"});
    CHECK(glsa.fingerprint == "2 dev-libs/foo-2");
    const auto& missing = list.at(1);
    CHECK(missing.title == "app-misc/a-1 needs libraries nothing installed provides");
    CHECK(missing.detail ==
          std::vector<std::string>{"libgone.so.2 (x86_64)", "libold.so.3 (x86_64)"});
    const auto& preserved = list.at(2);
    CHECK(preserved.title == "1 preserved library");
    CHECK(preserved.detail ==
          std::vector<std::string>{"/usr/lib/libfoo.so.1, used by app-misc/bar-1, app-misc/baz-1"});
    const auto& masked = list.at(3);
    CHECK(masked.title == "app-misc/b-1 is masked");
    CHECK(masked.detail == std::vector<std::string>{"package.mask", "~x86 keyword"});
    const auto& stale = list.at(4);
    CHECK(stale.title == "gentoo synced 8 days ago");
    CHECK(stale.fingerprint ==
          std::to_string(sys_seconds{sys_days{2026y / October / 1}}.time_since_epoch().count()));
    const auto& config = list.at(5);
    CHECK(config.title == "2 configuration files have updates waiting");
    CHECK(config.detail == std::vector<std::string>{"/etc/a.conf", "/etc/b.conf"});
    CHECK(list.at(6).title == "X happened");
    // An item without a title goes by its name.
    CHECK(list.at(7).title == "2026-09-02-y");
}

TEST_CASE("a fingerprint changes with what the notice says") {
    using namespace std::chrono;
    const egraph::Seconds now = sys_days{2026y / October / 9};
    auto notices = every_kind();
    const auto before = egraph::notice_list(notices, now);
    notices.advisories.front().revision = 3;
    notices.masked.front().reasons = {"package.mask"};
    notices.config.pop_back();
    const auto after = egraph::notice_list(notices, now);
    REQUIRE(before.size() == after.size());
    std::vector<std::string> changed;
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(before.at(i).key == after.at(i).key);
        if (before.at(i).fingerprint != after.at(i).fingerprint) {
            changed.push_back(after.at(i).key);
        }
    }
    CHECK(changed == std::vector<std::string>{"glsa:202601-01", "masked:app-misc/b-1", "config"});
}

TEST_CASE("since carries over by key, and new notices are those with new keys") {
    using namespace std::chrono;
    const egraph::Seconds then = sys_days{2026y / October / 1};
    const egraph::Seconds now = sys_days{2026y / October / 9};
    const std::vector<egraph::Notice> previous{{.kind = egraph::NoticeKind::news,
                                                .key = "news:gentoo/a",
                                                .title = "A",
                                                .detail = {},
                                                .fingerprint = "a",
                                                .since = then},
                                               {.kind = egraph::NoticeKind::masked,
                                                .key = "masked:x/y-1",
                                                .title = "gone",
                                                .detail = {},
                                                .fingerprint = "",
                                                .since = then}};
    std::vector<egraph::Notice> notices{{.kind = egraph::NoticeKind::news,
                                         .key = "news:gentoo/a",
                                         .title = "A",
                                         .detail = {},
                                         .fingerprint = "a",
                                         .since = now},
                                        {.kind = egraph::NoticeKind::news,
                                         .key = "news:gentoo/b",
                                         .title = "B",
                                         .detail = {},
                                         .fingerprint = "b",
                                         .since = now}};
    egraph::carry_since(notices, previous);
    CHECK(notices.at(0).since == then);
    CHECK(notices.at(1).since == now);
    const auto added = egraph::new_notices(notices, previous);
    REQUIRE(added.size() == 1);
    CHECK(added.front().key == "news:gentoo/b");
}

TEST_CASE("notice kinds have names") {
    for (const auto kind :
         {egraph::NoticeKind::glsa, egraph::NoticeKind::news, egraph::NoticeKind::config,
          egraph::NoticeKind::preserved, egraph::NoticeKind::stale, egraph::NoticeKind::masked,
          egraph::NoticeKind::missing}) {
        CHECK(egraph::notice_kind(egraph::notice_kind_name(kind)) == kind);
    }
    CHECK(egraph::notice_kind_name(egraph::NoticeKind::glsa) == "glsa");
    CHECK_FALSE(egraph::notice_kind("other"));
}

namespace {

egraph::Notice named(std::string key, std::string fingerprint = "1") {
    return {.kind = egraph::NoticeKind::news,
            .key = std::move(key),
            .title = "",
            .detail = {},
            .fingerprint = std::move(fingerprint),
            .since = {}};
}

} // namespace

TEST_CASE("what a user set aside round-trips through JSON") {
    using namespace std::chrono;
    const std::vector<egraph::SetAside> set_aside{
        {.key = "glsa:202601-01", .fingerprint = "2 a/b-1", .until = std::nullopt},
        {.key = "stale:gentoo",
         .fingerprint = "1790000000",
         .until = egraph::Seconds{seconds{1790086400}}}};
    CHECK(egraph::parse_set_aside(egraph::set_aside_json(set_aside)) == set_aside);
    CHECK(egraph::parse_set_aside("{}").error() == "not a set-aside file");
    CHECK(egraph::parse_set_aside(R"({"format": 1, "set_aside": [{"key": 1}]})").error() ==
          "not a set-aside file");
    CHECK(egraph::parse_set_aside(R"({"format": 2, "set_aside": []})").error() ==
          "format 2, from another egraph version");
}

TEST_CASE("set-aside notices live under the user's state directory") {
    CHECK(egraph::set_aside_path("/state", "/home/u") == "/state/egraph/set-aside.json");
    CHECK(egraph::set_aside_path(std::nullopt, "/home/u") ==
          "/home/u/.local/state/egraph/set-aside.json");
    // An empty or relative XDG_STATE_HOME counts as unset, as the specification says.
    CHECK(egraph::set_aside_path("", "/home/u") == "/home/u/.local/state/egraph/set-aside.json");
    CHECK(egraph::set_aside_path("state", "/home/u") ==
          "/home/u/.local/state/egraph/set-aside.json");
    CHECK_FALSE(egraph::set_aside_path(std::nullopt, std::nullopt));
}

TEST_CASE("a dismissed notice stays hidden until it changes; a put-off one until its time") {
    using namespace std::chrono;
    const egraph::Seconds now = sys_days{2026y / October / 9};
    std::vector<egraph::SetAside> set_aside;
    const std::vector<egraph::Notice> notices{named("a"), named("b"), named("c")};
    egraph::set_notice_aside(set_aside, notices.at(0), std::nullopt, notices);
    egraph::set_notice_aside(set_aside, notices.at(1), now + days{1}, notices);
    CHECK(egraph::is_set_aside(notices.at(0), set_aside, now + days{400}));
    CHECK_FALSE(egraph::is_set_aside(named("a", "2"), set_aside, now));
    CHECK(egraph::is_set_aside(notices.at(1), set_aside, now + hours{23}));
    CHECK_FALSE(egraph::is_set_aside(notices.at(1), set_aside, now + days{1}));
    CHECK_FALSE(egraph::is_set_aside(named("b", "2"), set_aside, now));
    CHECK_FALSE(egraph::is_set_aside(notices.at(2), set_aside, now));
    const auto shown = egraph::shown_notices(notices, set_aside, now);
    REQUIRE(shown.size() == 1);
    CHECK(shown.front().key == "c");
    // Dismissing a put-off notice replaces its entry; one whose notice is gone is dropped.
    egraph::set_notice_aside(set_aside, notices.at(1), std::nullopt,
                             std::vector{notices.at(1), notices.at(2)});
    CHECK(set_aside ==
          std::vector<egraph::SetAside>{{.key = "b", .fingerprint = "1", .until = std::nullopt}});
}

TEST_CASE("a notice is named by its key or what follows the colon") {
    const std::vector<egraph::Notice> notices{
        named("glsa:202601-01"), named("missing:app-misc/a-1"), named("masked:app-misc/a-1"),
        named("news:gentoo/2026-09-01-x"), named("config")};
    CHECK(egraph::named_notice(notices, "202601-01") == 0);
    CHECK(egraph::named_notice(notices, "masked:app-misc/a-1") == 2);
    CHECK(egraph::named_notice(notices, "gentoo/2026-09-01-x") == 3);
    CHECK(egraph::named_notice(notices, "config") == 4);
    CHECK(egraph::named_notice(notices, "app-misc/a-1").error() ==
          "app-misc/a-1 names 2 notices: missing:app-misc/a-1, masked:app-misc/a-1");
    CHECK(egraph::named_notice(notices, "nothing").error() == "no notice is named nothing");
}

TEST_CASE("notices less what keys name, for showing") {
    using namespace std::chrono;
    auto notices = every_kind();
    egraph::drop_notices(notices, std::vector<std::string>{"glsa:202601-01", "missing:app-misc/a-1",
                                                           "preserved", "masked:app-misc/b-1",
                                                           "stale:gentoo", "config",
                                                           "news:gentoo/2026-09-01-x"});
    CHECK(notices.advisories.empty());
    CHECK(notices.missing.empty());
    CHECK(notices.preserved->empty());
    CHECK(notices.rebuild->empty());
    CHECK(notices.masked.empty());
    CHECK(notices.stale.empty());
    CHECK(notices.config.empty());
    REQUIRE(notices.news.size() == 1);
    CHECK(notices.news.front().item == "2026-09-02-y");
}
