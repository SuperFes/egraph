#include "notices.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("notice lines give configuration updates, then news") {
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
        .rebuild = std::vector<std::string>{"app-misc/bar:0", "app-misc/baz:0"}};
    CHECK(egraph::notice_lines(notices) ==
          std::vector<std::string>{
              "/etc/a.conf\tconfig\t/etc/._cfg0000_a.conf",
              "/etc/a.conf\tconfig\t/etc/._cfg0001_a.conf",
              "2026-09-01-x\tnews\tgentoo\tX happened",
              "/usr/lib/libfoo.so.1\tpreserved\tdev-libs/foo-2\tapp-misc/bar-1 app-misc/baz-1",
              "/usr/lib/libold.so.1\tpreserved\tdev-libs/old-2\t", "app-misc/bar:0\trebuild",
              "app-misc/baz:0\trebuild"});
}
