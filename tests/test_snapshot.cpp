#include "snapshot.hpp"

#include "action.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace snapshot = egraph::snapshot;
using Argv = std::vector<std::string>;
using Binds = std::vector<egraph::os::Bind>;

TEST_CASE("btrfs commands name the subvolume and the snapshot") {
    CHECK(snapshot::show_command("/") == Argv{"btrfs", "subvolume", "show", "/"});
    CHECK(snapshot::create_command("/", "/var/tmp/egraph/root") ==
          Argv{"btrfs", "subvolume", "snapshot", "/", "/var/tmp/egraph/root"});
    CHECK(snapshot::remove_command("/var/tmp/egraph/root") ==
          Argv{"btrfs", "subvolume", "delete", "/var/tmp/egraph/root"});
}

TEST_CASE("a directory that is not a subvolume is not snapshottable") {
    CHECK_FALSE(snapshot::snapshottable("/nonexistent/egraph"));
}

TEST_CASE("creating a snapshot of what is not a subvolume fails with a reason") {
    const auto made = snapshot::create("/nonexistent/egraph", "/nonexistent/egraph-snapshot");
    REQUIRE_FALSE(made.has_value());
    CHECK_FALSE(made.error().empty());
}

TEST_CASE("the flags override the snapshot setting") {
    using egraph::SnapshotRun;
    CHECK(snapshot::asked(SnapshotRun::automatic, std::nullopt) == SnapshotRun::automatic);
    CHECK(snapshot::asked(SnapshotRun::off, true) == SnapshotRun::on);
    CHECK(snapshot::asked(SnapshotRun::on, false) == SnapshotRun::off);
}

TEST_CASE("a run goes in a snapshot only where one can be taken, saying why not") {
    using egraph::SnapshotRun;
    const snapshot::Facts able{.snapshottable = true, .root = true, .free = 100, .needed = 60};
    CHECK(snapshot::decide(SnapshotRun::off, std::nullopt, able) ==
          snapshot::Decision{snapshot::Plain{}});
    CHECK(snapshot::decide(SnapshotRun::automatic, std::nullopt, able) ==
          snapshot::Decision{snapshot::InSnapshot{}});
    CHECK(snapshot::decide(SnapshotRun::off, true, able) ==
          snapshot::Decision{snapshot::InSnapshot{}});
    CHECK(snapshot::decide(SnapshotRun::on, false, able) == snapshot::Decision{snapshot::Plain{}});

    auto unable = able;
    unable.snapshottable = false;
    CHECK(snapshot::decide(SnapshotRun::on, std::nullopt, unable) ==
          snapshot::Decision{snapshot::Refused{.why = "the root is not a btrfs subvolume"}});
    CHECK(snapshot::decide(SnapshotRun::automatic, std::nullopt, unable) ==
          snapshot::Decision{snapshot::Plain{.why = "the root is not a btrfs subvolume"}});
    unable = able;
    unable.root = false;
    CHECK(snapshot::decide(SnapshotRun::on, std::nullopt, unable) ==
          snapshot::Decision{snapshot::Refused{.why = "only root can take a snapshot"}});
    unable = able;
    unable.needed = 101;
    CHECK(snapshot::decide(SnapshotRun::automatic, std::nullopt, unable) ==
          snapshot::Decision{
              snapshot::Plain{.why = "the snapshot may take 101 bytes, and 100 are free"}});
}

TEST_CASE("a snapshot run may take twice the merges' installed sizes") {
    const std::vector<std::optional<std::uint64_t>> sizes{10, std::nullopt, 5};
    CHECK(snapshot::needed_bytes(sizes) == 30);
    CHECK(snapshot::needed_bytes({}) == 0);
    const std::vector<std::optional<std::uint64_t>> huge{std::numeric_limits<std::uint64_t>::max(),
                                                         1};
    CHECK(snapshot::needed_bytes(huge) == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("binds go each directory before those inside it, a twice-given one writable") {
    CHECK(snapshot::ordered({{.path = "/var/db/repos/gentoo/distfiles", .read_only = false},
                             {.path = "/var/db/repos/gentoo/", .read_only = true},
                             {.path = "/var/tmp", .read_only = true},
                             {.path = "/var//tmp/.", .read_only = false},
                             {.path = "/run", .read_only = false}}) ==
          Binds{{.path = "/run", .read_only = false},
                {.path = "/var/db/repos/gentoo", .read_only = true},
                {.path = "/var/db/repos/gentoo/distfiles", .read_only = false},
                {.path = "/var/tmp", .read_only = false}});
    CHECK(snapshot::ordered({}).empty());
}

TEST_CASE("a worker in a snapshot shares the kernel's filesystems, builds, fetches and caches") {
    egraph::RunSettings settings;
    settings.tmpdir = "/var/tmp";
    settings.distdir = "/var/cache/distfiles";
    settings.repositories = {"/var/db/repos/gentoo", "/srv/overlay"};
    const Binds without_ccache{{.path = "/dev", .read_only = false},
                               {.path = "/proc", .read_only = false},
                               {.path = "/run", .read_only = false},
                               {.path = "/srv/overlay", .read_only = true},
                               {.path = "/sys", .read_only = false},
                               {.path = "/var/cache/distfiles", .read_only = false},
                               {.path = "/var/db/repos/gentoo", .read_only = true},
                               {.path = "/var/tmp", .read_only = false}};
    CHECK(snapshot::binds(settings) == without_ccache);
    settings.ccache_dir = "/var/cache/ccache";
    const auto with_ccache = snapshot::binds(settings);
    CHECK(std::ranges::find(with_ccache,
                            egraph::os::Bind{.path = "/var/cache/ccache", .read_only = false}) !=
          with_ccache.end());
    CHECK(with_ccache.size() == without_ccache.size() + 1);
}

TEST_CASE("a worker enters the snapshot through egraph enter") {
    const Binds binds{{.path = "/run", .read_only = false},
                      {.path = "/var/db/repos/gentoo", .read_only = true}};
    CHECK(snapshot::enter_command("/usr/bin/egraph", "/var/tmp/egraph/root", binds,
                                  Argv{"/usr/bin/egraph-build", "--worker", "--root", "/"}) ==
          Argv{"/usr/bin/egraph", "enter", "--bind", "/run", "--read-only", "/var/db/repos/gentoo",
               "/var/tmp/egraph/root", "--", "/usr/bin/egraph-build", "--worker", "--root", "/"});
}
