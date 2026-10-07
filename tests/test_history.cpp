#include "history.hpp"

#include "helpers.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using egraph::Seconds;
using egraph::test::Installed;
using egraph::test::make_system;
using egraph::test::TempDir;
using namespace std::chrono_literals;

namespace {

constexpr Seconds at(std::chrono::year_month_day day, std::chrono::seconds time = 0s) {
    return std::chrono::sys_days{day} + time;
}

constexpr auto oct = std::chrono::October;

} // namespace

TEST_CASE("settings default to 90 days of history") {
    CHECK(egraph::parse_settings("")->history_days == 90);
    CHECK(egraph::parse_settings("# none\n\n")->history_days == 90);
}

TEST_CASE("settings are key = value lines with comments") {
    CHECK(egraph::parse_settings("history_days = 30\n")->history_days == 30);
    CHECK(egraph::parse_settings("  history_days=0  # off\n")->history_days == 0);
}

TEST_CASE("a setting egraph does not know, or a bad value, names its line") {
    CHECK(egraph::parse_settings("\nhistory = 3\n").error() == "line 2: unknown setting history");
    CHECK(egraph::parse_settings("history_days = soon").error() ==
          "line 1: history_days takes a number of days, not soon");
    CHECK(egraph::parse_settings("history_days = -1").error() ==
          "line 1: history_days takes a number of days, not -1");
    CHECK(egraph::parse_settings("history_days").error() == "line 1: expected key = value");
}

TEST_CASE("settings come from etc/egraph/egraph.conf, the defaults without one") {
    const TempDir dir;
    CHECK(egraph::settings_path(dir.path()) == dir.path() / "etc/egraph/egraph.conf");
    CHECK(egraph::read_settings(dir.path() / "egraph.conf")->history_days == 90);
    std::ofstream{dir.path() / "egraph.conf"} << "history_days = 7\n";
    CHECK(egraph::read_settings(dir.path() / "egraph.conf")->history_days == 7);
    std::ofstream{dir.path() / "egraph.conf"} << "days = 7\n";
    CHECK(egraph::read_settings(dir.path() / "egraph.conf").error() ==
          (dir.path() / "egraph.conf").string() + ": line 1: unknown setting days");
}

TEST_CASE("history lives in var/lib/egraph under the root") {
    CHECK(egraph::history_directory("/", "") == "/var/lib/egraph");
    CHECK(egraph::history_directory("/mnt/gentoo", "/prefix") ==
          "/mnt/gentoo/prefix/var/lib/egraph");
}

TEST_CASE("a generation is named by when its store was built, in UTC") {
    const auto built = at(2026y / oct / 6d, 16h + 26min + 5s);
    CHECK(egraph::generation_name(built) == "installed-20261006T162605Z.egraph");
    CHECK(egraph::generation_time("installed-20261006T162605Z.egraph") == built);
}

TEST_CASE("other names are not generations") {
    for (const auto* name :
         {"installed.egraph", "installed-20261006T162605Z.egraph.tmp",
          ".installed-20261006T162605Z.egraph.1234abcd", "installed-2026106T162605Z.egraph",
          "installed-20261306T162605Z.egraph", "history.log"}) {
        CAPTURE(name);
        CHECK_FALSE(egraph::generation_time(name).has_value());
    }
}

TEST_CASE("thinning keeps the last day whole") {
    const auto now = at(2026y / oct / 6d, 12h);
    const std::vector<Seconds> recent{now - 23h, now - 22h, now - 1h, now - 1min};
    CHECK(egraph::thinned(recent, now, 90).empty());
}

TEST_CASE("thinning keeps the newest of each older day") {
    const auto now = at(2026y / oct / 6d, 12h);
    const std::vector<Seconds> generations{at(2026y / oct / 4d, 9h), at(2026y / oct / 4d, 23h),
                                           at(2026y / oct / 4d, 10h), at(2026y / oct / 1d, 1h),
                                           now - 2h};
    CHECK(egraph::thinned(generations, now, 90) ==
          std::vector<Seconds>{at(2026y / oct / 4d, 9h), at(2026y / oct / 4d, 10h)});
}

TEST_CASE("thinning drops generations past the days kept") {
    const auto now = at(2026y / oct / 6d, 12h);
    const std::vector<Seconds> generations{at(2026y / std::chrono::July / 7d, 13h),
                                           at(2026y / std::chrono::July / 8d, 13h)};
    CHECK(egraph::thinned(generations, now, 90) == std::vector<Seconds>{generations.front()});
    CHECK(egraph::thinned(generations, now, 0) == generations);
}

TEST_CASE("history notices installed packages that came, went or changed") {
    const auto before = make_system({{.cpv = "app-misc/a-1"}, {.cpv = "dev-libs/b-1"}}, {});
    const auto added = make_system(
        {{.cpv = "app-misc/a-1"}, {.cpv = "dev-libs/b-1"}, {.cpv = "dev-libs/c-1"}}, {});
    const auto removed = make_system({{.cpv = "app-misc/a-1"}}, {});
    const auto rebuilt = make_system(
        {{.cpv = "app-misc/a-1", .iuse = "x", .use = "x"}, {.cpv = "dev-libs/b-1"}}, {});
    CHECK(egraph::history_changed(before.store, added.store));
    CHECK(egraph::history_changed(before.store, removed.store));
    CHECK(egraph::history_changed(before.store, rebuilt.store));
}

TEST_CASE("history notices a root set that changed") {
    const std::vector<Installed> installed{{.cpv = "app-misc/a-1"}, {.cpv = "dev-libs/b-1"}};
    const auto before = make_system(installed, {}, {"app-misc/a"});
    CHECK(egraph::history_changed(before.store, make_system(installed, {}, {"dev-libs/b"}).store));
    CHECK(egraph::history_changed(before.store, make_system(installed, {}).store));
    CHECK(egraph::history_changed(
        before.store, make_system(installed, {}, {"app-misc/a"}, {"dev-libs/b"}).store));
}

TEST_CASE("history ignores the same system in other string ids, inputs and meta") {
    const auto before =
        make_system({{.cpv = "app-misc/a-1"}, {.cpv = "dev-libs/b-1"}}, {}, {"app-misc/a"});
    // Interned in another order.
    auto after =
        make_system({{.cpv = "dev-libs/b-1"}, {.cpv = "app-misc/a-1"}}, {}, {"app-misc/a"});
    after.store.meta.build_time_ns = 5;
    after.store.inputs.push_back({.path = "/var/db/pkg"});
    CHECK_FALSE(egraph::history_changed(before.store, after.store));
}
