#include "notices.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("notices are read from egraph-build's JSON") {
    const auto notices = egraph::parse_notices(R"({
 "config": [{"file": "/etc/a.conf", "update": "/etc/._cfg0000_a.conf"}],
 "news": [{"item": "2026-09-01-x", "repo": "gentoo", "title": "X happened"}]
})");
    REQUIRE(notices);
    REQUIRE(notices->config.size() == 1);
    CHECK(notices->config.front().file == "/etc/a.conf");
    CHECK(notices->config.front().update == "/etc/._cfg0000_a.conf");
    REQUIRE(notices->news.size() == 1);
    CHECK(notices->news.front().repo == "gentoo");
    CHECK(notices->news.front().item == "2026-09-01-x");
    CHECK(notices->news.front().title == "X happened");
}

TEST_CASE("malformed notices are an error") {
    CHECK(!egraph::parse_notices("[]"));
    CHECK(!egraph::parse_notices("{"));
    CHECK(!egraph::parse_notices(R"({"config": [{"file": 1}], "news": []})"));
    CHECK(!egraph::parse_notices(R"({"config": [], "news": [{"repo": "gentoo"}]})"));
    CHECK(!egraph::parse_notices(R"({"config": {}, "news": []})"));
    const auto empty = egraph::parse_notices(R"({"config": [], "news": []})");
    REQUIRE(empty);
    CHECK(egraph::notice_lines(*empty).empty());
}

TEST_CASE("notice lines give configuration updates, then news") {
    const egraph::Notices notices{
        .config = {{.file = "/etc/a.conf", .update = "/etc/._cfg0000_a.conf"},
                   {.file = "/etc/a.conf", .update = "/etc/._cfg0001_a.conf"}},
        .news = {{.repo = "gentoo", .item = "2026-09-01-x", .title = "X happened"}}};
    CHECK(egraph::notice_lines(notices) ==
          std::vector<std::string>{"/etc/a.conf\tconfig\t/etc/._cfg0000_a.conf",
                                   "/etc/a.conf\tconfig\t/etc/._cfg0001_a.conf",
                                   "2026-09-01-x\tnews\tgentoo\tX happened"});
}
