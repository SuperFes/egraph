#include "emerge_log.hpp"

#include "estimate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::BuildRecord;

namespace {

// Two sessions: the first at --jobs=3 builds a and b beside each other, merges c as a binary
// package, fails d and builds e into another root; the second, at emerge's one job, builds f.
constexpr std::string_view log = R"(1000: Started emerge on: Oct 01, 2026 10:00:00
1000:  *** emerge --jobs=3 --keep-going --update app-misc/a
1001:  >>> emerge (1 of 5) app-misc/a-1 to /
1001:  === (1 of 5) Cleaning (app-misc/a-1::/var/db/repos/gentoo/app-misc/a/a-1.ebuild)
1001:  === (1 of 5) Compiling/Merging (app-misc/a-1::/var/db/repos/gentoo/app-misc/a/a-1.ebuild)
1002:  >>> emerge (2 of 5) app-misc/b-2 to /
1002:  === (2 of 5) Compiling/Packaging (app-misc/b-2::/var/db/repos/gentoo/app-misc/b/b-2.ebuild)
1050:  === (1 of 5) Merging (app-misc/a-1::/var/db/repos/gentoo/app-misc/a/a-1.ebuild)
1055:  >>> AUTOCLEAN: app-misc/a:0
1058:  === (1 of 5) Post-Build Cleaning (app-misc/a-1::/var/db/repos/gentoo/app-misc/a/a-1.ebuild)
1058:  ::: completed emerge (1 of 5) app-misc/a-1 to /
1100:  >>> emerge (3 of 5) app-misc/c-1 to /
1101:  === (3 of 5) Merging Binary (app-misc/c-1::/var/cache/binpkgs/app-misc/c/c-1-1.gpkg.tar)
1110:  ::: completed emerge (3 of 5) app-misc/c-1 to /
1120:  === (2 of 5) Merging (app-misc/b-2::/var/db/repos/gentoo/app-misc/b/b-2.ebuild)
1122:  ::: completed emerge (2 of 5) app-misc/b-2 to /
1130:  >>> emerge (4 of 5) app-misc/d-1 to /
1130:  === (4 of 5) Compiling/Merging (app-misc/d-1::/var/db/repos/gentoo/app-misc/d/d-1.ebuild)
1140:  >>> emerge (5 of 5) app-misc/e-1 to /mnt/other/
1140:  === (5 of 5) Compiling/Merging (app-misc/e-1::/var/db/repos/gentoo/app-misc/e/e-1.ebuild)
1150:  === (5 of 5) Merging (app-misc/e-1::/var/db/repos/gentoo/app-misc/e/e-1.ebuild)
1151:  ::: completed emerge (5 of 5) app-misc/e-1 to /mnt/other/
1160:  *** exiting unsuccessfully with status '1'.
2000: Started emerge on: Oct 01, 2026 10:16:40
2000:  *** emerge --oneshot app-misc/f
2001:  >>> emerge (1 of 1) app-misc/f-1 to /
2001:  === (1 of 1) Compiling/Merging (app-misc/f-1::/var/db/repos/gentoo/app-misc/f/f-1.ebuild)
not a line of emerge's
2100:  === (1 of 1) Merging (app-misc/f-1::/var/db/repos/gentoo/app-misc/f/f-1.ebuild)
2103:  ::: completed emerge (1 of 1) app-misc/f-1 to /
2104:  *** exiting successfully.
3000:  *** emerge --jobs --oneshot app-misc/g
3001:  >>> emerge (1 of 1) app-misc/g-1 to /
3001:  === (1 of 1) Compiling/Merging (app-misc/g-1::/var/db/repos/gentoo/app-misc/g/g-1.ebuild)
3011:  === (1 of 1) Merging (app-misc/g-1::/var/db/repos/gentoo/app-misc/g/g-1.ebuild)
3012:  ::: completed emerge (1 of 1) app-misc/g-1 to /
)";

BuildRecord imported(std::string cpv, double start, double seconds, double merge,
                     std::optional<std::uint32_t> jobs) {
    return {.origin = std::string{egraph::emerge_log_origin},
            .start = start,
            .cpv = std::move(cpv),
            .build_seconds = seconds,
            .merge_seconds = merge,
            .jobs = jobs};
}

} // namespace

TEST_CASE("emerge.log's compiled builds become records, from start to merge to completion") {
    CHECK(egraph::emerge_log_builds(log, "/") ==
          std::vector<BuildRecord>{imported("app-misc/a-1", 1001, 49, 8, 3),
                                   imported("app-misc/b-2", 1002, 118, 2, 3),
                                   imported("app-misc/f-1", 2001, 99, 3, 1),
                                   imported("app-misc/g-1", 3001, 10, 1, std::nullopt)});
    // Another root's, as emerge names it.
    CHECK(egraph::emerge_log_builds(log, "/mnt/other/") ==
          std::vector<BuildRecord>{imported("app-misc/e-1", 1140, 10, 1, 3)});
    CHECK(egraph::emerge_log_builds("", "/").empty());
    CHECK(egraph::emerge_log_path("/prefix") == "/prefix/var/log/emerge.log");
}

TEST_CASE("imported builds join the history in order of start, those it holds left out") {
    const auto own = [](std::string cpv, double start) {
        return egraph::build_record_json(
            {.origin = "exec", .start = start, .cpv = std::move(cpv), .build_seconds = 5});
    };
    const auto history =
        own("app-misc/a-1", 1030) + "\n" + "{\"version\": 99}\n" + own("app-misc/f-1", 2500) + "\n";
    const std::vector<BuildRecord> records{imported("app-misc/a-1", 1001, 49, 8, 3),
                                           imported("app-misc/b-2", 1002, 118, 2, 3),
                                           imported("app-misc/f-1", 2001, 99, 3, 1)};
    const auto merged = egraph::imported_builds(history, records, 20);
    // a's build is the one exec recorded, within a minute.
    CHECK(merged.added == 2);
    CHECK(merged.text == "{\"version\": 99}\n" + egraph::build_record_json(records.at(1)) + "\n" +
                             own("app-misc/a-1", 1030) + "\n" +
                             egraph::build_record_json(records.at(2)) + "\n" +
                             own("app-misc/f-1", 2500) + "\n");
    // Imported again, nothing more.
    const auto again = egraph::imported_builds(merged.text, records, 20);
    CHECK(again.added == 0);
    CHECK(again.text == merged.text);
    // Each cp kept to its last builds.
    const auto kept = egraph::imported_builds(history, records, 1);
    CHECK(kept.text.find(egraph::build_record_json(records.at(2))) == std::string::npos);
    CHECK(kept.text.find(own("app-misc/f-1", 2500)) != std::string::npos);
}

TEST_CASE("imported builds count only for packages egraph never timed itself") {
    const std::vector<BuildRecord> records{
        imported("app-misc/a-1", 1001, 49, 8, 3),
        {.origin = "exec", .start = 1500, .cpv = "app-misc/a-1", .build_seconds = 10, .jobs = 2},
        imported("app-misc/b-2", 1002, 118, 2, 3),
    };
    const egraph::BuildKnowledge knowledge{records};
    CHECK(knowledge.estimate("app-misc/a-1", 4)->seconds == 10);
    CHECK(knowledge.estimate("app-misc/a-1", 4)->builds == 1);
    CHECK(knowledge.estimate("app-misc/b-2", 4)->seconds == 118);
    // The jobs of egraph's own latest build, over an imported one's after it.
    CHECK(egraph::recorded_jobs(records) == 2U);
    CHECK(egraph::recorded_jobs({records.at(2)}) == 3U);
}
