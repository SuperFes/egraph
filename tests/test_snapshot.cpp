#include "snapshot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace snapshot = egraph::snapshot;
using Argv = std::vector<std::string>;

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
          snapshot::Decision{snapshot::Plain{
              .why = "the snapshot may take 101 bytes, and 100 are free"}});
}

TEST_CASE("a snapshot run may take twice the merges' installed sizes") {
    const std::vector<std::optional<std::uint64_t>> sizes{10, std::nullopt, 5};
    CHECK(snapshot::needed_bytes(sizes) == 30);
    CHECK(snapshot::needed_bytes({}) == 0);
    const std::vector<std::optional<std::uint64_t>> huge{
        std::numeric_limits<std::uint64_t>::max(), 1};
    CHECK(snapshot::needed_bytes(huge) == std::numeric_limits<std::uint64_t>::max());
}
