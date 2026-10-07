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
