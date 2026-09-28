#include "cli.hpp"
#include "freshness.hpp"
#include "helpers.hpp"
#include "os.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using egraph::test::fake_builder;
using egraph::test::fresh_sample;
using egraph::test::read_text;
using egraph::test::TempDir;
using egraph::test::write_bytes;
using egraph::test::write_text;

namespace {

constexpr std::uint64_t second_ns = 1'000'000'000;

egraph::Input recorded(const fs::path& path, egraph::InputKind kind) {
    const auto status = egraph::os::lstat(path);
    REQUIRE(status.has_value());
    return {
        .path = path.string(), .kind = kind, .mtime_ns = status->mtime_ns, .size = status->size};
}

// A store built comfortably after its inputs last changed.
egraph::Store store_of(std::vector<egraph::Input> inputs) {
    egraph::Store store;
    std::uint64_t latest = 0;
    for (const auto& input : inputs) {
        latest = std::max(latest, input.mtime_ns);
    }
    store.meta.build_time_ns = latest + (2 * second_ns);
    store.inputs = std::move(inputs);
    return store;
}

} // namespace

TEST_CASE("unchanged inputs are fresh") {
    const TempDir dir;
    write_text(dir.path() / "file", "abc");
    fs::create_symlink("file", dir.path() / "link");
    const auto store =
        store_of({recorded(dir.path(), egraph::InputKind::directory),
                  recorded(dir.path() / "file", egraph::InputKind::file),
                  recorded(dir.path() / "link", egraph::InputKind::symlink),
                  {.path = (dir.path() / "absent").string(), .kind = egraph::InputKind::missing}});
    CHECK_FALSE(egraph::staleness(store).has_value());
}

TEST_CASE("changed inputs are stale") {
    const TempDir dir;
    const auto file = dir.path() / "file";
    write_text(file, "abc");
    const auto store = store_of({recorded(file, egraph::InputKind::file)});

    SECTION("size") {
        write_text(file, "abcd");
        CHECK(egraph::staleness(store) == file.string() + ": changed");
    }
    SECTION("mtime") {
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
        CHECK(egraph::staleness(store) == file.string() + ": changed");
    }
    SECTION("kind") {
        fs::remove(file);
        fs::create_directory(file);
        CHECK(egraph::staleness(store).has_value());
    }
    SECTION("removed") {
        fs::remove(file);
        CHECK(egraph::staleness(store) == file.string() + ": No such file or directory");
    }
}

TEST_CASE("a recorded absence is stale once the path exists") {
    const TempDir dir;
    const auto path = dir.path() / "later";
    const auto store = store_of({{.path = path.string(), .kind = egraph::InputKind::missing}});
    write_text(path, "");
    CHECK(egraph::staleness(store) == path.string() + ": created");
}

TEST_CASE("inputs modified close to the build are not trusted") {
    const TempDir dir;
    write_text(dir.path() / "file", "abc");
    auto store = store_of({recorded(dir.path() / "file", egraph::InputKind::file)});
    store.meta.build_time_ns = store.inputs.front().mtime_ns + (second_ns / 2);
    CHECK_THAT(egraph::staleness(store).value_or(""),
               EndsWith("modified too close to the build to trust"));
}

TEST_CASE("processes run and report how they ended") {
    CHECK(egraph::os::run({"true"}) == 0);
    CHECK(egraph::os::run({"false"}) == 1);
    CHECK(egraph::os::run({"sh", "-c", "exit 7"}) == 7);
    const auto missing = egraph::os::run({"/nonexistent/egraph-build"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message == "/nonexistent/egraph-build: No such file or directory");
    const auto killed = egraph::os::run({"sh", "-c", "kill -KILL $$"});
    REQUIRE_FALSE(killed.has_value());
    CHECK(killed.error().message == "sh: killed by signal 9");
}

TEST_CASE("a process's output can go to a log instead of our terminal") {
    const TempDir dir;
    const auto log = dir.path() / "log";
    write_text(log, "left over");
    CHECK(egraph::os::run({"sh", "-c", "echo out; echo err >&2; cat"}, log) == 0);
    CHECK(read_text(log) == "out\nerr\n");
    const auto unwritable = egraph::os::run({"true"}, dir.path() / "missing" / "log");
    REQUIRE_FALSE(unwritable.has_value());
}

TEST_CASE("the refresh command passes the roots through") {
    egraph::Invocation invocation;
    invocation.builder = "/usr/bin/egraph-build";
    CHECK(egraph::builder_command(invocation, "--incremental", "/s") ==
          std::vector<std::string>{"/usr/bin/egraph-build", "--incremental", "--store", "/s",
                                   "--root", "/"});
    invocation.root = "/mnt";
    invocation.config_root = "/cfg";
    invocation.eprefix = "/prefix";
    CHECK(egraph::builder_command(invocation, "--incremental", "/s") ==
          std::vector<std::string>{"/usr/bin/egraph-build", "--incremental", "--store", "/s",
                                   "--root", "/mnt", "--config-root", "/cfg", "--eprefix",
                                   "/prefix"});
    CHECK(egraph::store_path(invocation) ==
          fs::path{"/mnt/prefix/var/cache/egraph/installed.egraph"});
}

namespace {

egraph::Invocation export_json(const fs::path& store, const fs::path& builder) {
    egraph::Invocation invocation;
    invocation.store = store;
    invocation.builder = builder.string();
    invocation.command = egraph::Export{.format = egraph::ExportFormat::json, .packages = {}};
    return invocation;
}

} // namespace

TEST_CASE("a missing store is built before answering") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    auto invocation = export_json(store, fake_builder(dir.path(), fresh_sample(), 0));
    invocation.eprefix = "/prefix";
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
    CHECK(err.str().empty());
    CHECK_THAT(out.str(), ContainsSubstring("app-misc/a-1"));
    CHECK(read_text(dir.path() / "args") ==
          std::format("--incremental --store {} --root / --eprefix /prefix\n", store.string()));
}

TEST_CASE("a stale store is refreshed before answering") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    // The sample records /var/db/pkg/app-misc with an mtime of 5 ns, which is never current.
    write_bytes(store, egraph::test::assemble(egraph::test::sample_sections()));
    const auto invocation = export_json(store, fake_builder(dir.path(), fresh_sample(), 0));
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
    CHECK(fs::exists(dir.path() / "args"));
    CHECK(egraph::load(store)->inputs.empty());
}

TEST_CASE("--no-refresh answers from a stale store with a warning") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    write_bytes(store, egraph::test::assemble(egraph::test::sample_sections()));
    auto invocation = export_json(store, fake_builder(dir.path(), fresh_sample(), 0));
    invocation.no_refresh = true;
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
    CHECK_FALSE(fs::exists(dir.path() / "args"));
    CHECK_THAT(err.str(), ContainsSubstring("warning: answering from a stale store"));
    CHECK_THAT(out.str(), ContainsSubstring("app-misc/a-1"));
}

TEST_CASE("--no-refresh without a store fails") {
    const TempDir dir;
    auto invocation =
        export_json(dir.path() / "none.egraph", fake_builder(dir.path(), fresh_sample(), 0));
    invocation.no_refresh = true;
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::failure);
    CHECK_THAT(err.str(), ContainsSubstring("No such file or directory"));
}

TEST_CASE("a failing builder fails the query") {
    const TempDir dir;
    const auto builder = fake_builder(dir.path(), fresh_sample(), 7);
    const auto invocation = export_json(dir.path() / "installed.egraph", builder);
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::failure);
    CHECK(err.str() == std::format("egraph: {} exited with status 7\n", builder.string()));
    CHECK(out.str().empty());
}

TEST_CASE("egraph-build is looked for next to egraph, then in PATH") {
    const TempDir dir;
    egraph::Invocation invocation;
    CHECK(egraph::builder_program(invocation) == "egraph-build");
    invocation.program_dir = dir.path();
    CHECK(egraph::builder_program(invocation) == "egraph-build");
    write_text(dir.path() / "egraph-build", "");
    CHECK(egraph::builder_program(invocation) == (dir.path() / "egraph-build").string());
    invocation.builder = "/opt/egraph-build";
    CHECK(egraph::builder_program(invocation) == "/opt/egraph-build");
}

TEST_CASE("the user's store is named after the root") {
    egraph::Invocation invocation;
    CHECK_FALSE(egraph::user_store_path(invocation).has_value());
    invocation.cache_home = "/home/u/.cache";
    CHECK(egraph::user_store_path(invocation) ==
          fs::path{"/home/u/.cache/egraph/installed.egraph"});
    invocation.root = "/mnt/gentoo";
    invocation.eprefix = "/prefix";
    CHECK(egraph::user_store_path(invocation) ==
          fs::path{"/home/u/.cache/egraph/installed-mnt-gentoo-prefix.egraph"});
}

namespace {

// A root whose system store directory only root could write, holding store.
struct ReadOnlyRoot {
    explicit ReadOnlyRoot(const fs::path& root, const std::vector<std::byte>& store)
        : directory(root / "var/cache/egraph") {
        fs::create_directories(directory);
        write_bytes(directory / "installed.egraph", store);
        fs::permissions(directory, fs::perms::owner_read | fs::perms::owner_exec);
    }
    ~ReadOnlyRoot() { fs::permissions(directory, fs::perms::owner_all); }
    ReadOnlyRoot(const ReadOnlyRoot&) = delete;
    ReadOnlyRoot& operator=(const ReadOnlyRoot&) = delete;
    ReadOnlyRoot(ReadOnlyRoot&&) = delete;
    ReadOnlyRoot& operator=(ReadOnlyRoot&&) = delete;

    fs::path directory;
};

} // namespace

TEST_CASE("a system store only root can write is read when current, else the user's is kept") {
    const TempDir dir;
    auto invocation = export_json({}, fake_builder(dir.path(), fresh_sample(), 0));
    invocation.store.reset();
    invocation.root = dir.path() / "root";
    invocation.cache_home = dir.path() / "cache";
    const auto user = egraph::user_store_path(invocation).value_or(fs::path{});
    fs::create_directories(invocation.root);
    CHECK(egraph::store_path(invocation) == egraph::system_store_path(invocation));

    SECTION("current") {
        const ReadOnlyRoot root{invocation.root, fresh_sample()};
        if (egraph::os::can_create(egraph::system_store_path(invocation))) {
            SKIP("running as root: every directory is writable");
        }
        CHECK(egraph::store_path(invocation) == user);
        std::ostringstream out;
        std::ostringstream err;
        REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
        CHECK_THAT(out.str(), ContainsSubstring("app-misc/a-1"));
        CHECK_FALSE(fs::exists(dir.path() / "args"));
    }
    SECTION("stale") {
        const ReadOnlyRoot root{invocation.root,
                                egraph::test::assemble(egraph::test::sample_sections())};
        if (egraph::os::can_create(egraph::system_store_path(invocation))) {
            SKIP("running as root: every directory is writable");
        }
        std::ostringstream out;
        std::ostringstream err;
        REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
        CHECK(read_text(dir.path() / "args") == std::format("--incremental --store {} --root {}\n",
                                                            user.string(),
                                                            invocation.root.string()));
        CHECK(egraph::load(user).has_value());
    }
}

TEST_CASE("a builder that cannot run says how to name one") {
    const TempDir dir;
    auto invocation = export_json(dir.path() / "installed.egraph", "/nonexistent/egraph-build");
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::failure);
    CHECK_THAT(err.str(), ContainsSubstring("cannot build the store: /nonexistent/egraph-build: "
                                            "No such file or directory (install egraph-build"));
}
