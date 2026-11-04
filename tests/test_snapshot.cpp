#include "snapshot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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
