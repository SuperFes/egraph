#include "check.hpp"
#include "cli.hpp"
#include "helpers.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using egraph::test::Bytes;
using egraph::test::fake_builder;
using egraph::test::fresh_sample;
using egraph::test::read_text;
using egraph::test::TempDir;
using egraph::test::with_section;
using egraph::test::write_bytes;

namespace {

// A store holding only dev-libs/b-1, with its slot given as a sample string id.
std::vector<std::byte> only_b(std::uint64_t slot) {
    Bytes packages;
    packages.varint(1).varints({8, 7, slot, slot, 4, 5}).list({}).list({}).varint(0);
    packages.varints({0, 0, 0, 0, 0}).varint(1).varints({11, 12}).varint(0);
    auto sections = egraph::test::sample_sections();
    sections.at(1).bytes = Bytes{}.varint(0).bytes();
    sections.at(3).bytes = packages.bytes();
    return egraph::test::assemble(sections);
}

egraph::Store decoded(const std::vector<std::byte>& bytes) {
    auto store = egraph::decode(bytes);
    REQUIRE(store.has_value());
    return std::move(*store);
}

egraph::Invocation command(const std::filesystem::path& store, const std::filesystem::path& builder,
                           egraph::Command which) {
    egraph::Invocation invocation;
    invocation.store = store;
    invocation.builder = builder.string();
    invocation.command = std::move(which);
    return invocation;
}

} // namespace

TEST_CASE("identical stores have no drift") {
    CHECK(egraph::drift(decoded(fresh_sample()), decoded(fresh_sample())).empty());
}

TEST_CASE("drift names each package that differs") {
    const auto both = decoded(fresh_sample());
    const auto b = decoded(only_b(3));
    CHECK(egraph::drift(both, b) == std::vector<std::string>{"-app-misc/a-1"});
    CHECK(egraph::drift(b, both) == std::vector<std::string>{"+app-misc/a-1"});
    // Slot "8" instead of "0".
    CHECK(egraph::drift(b, decoded(only_b(5))) == std::vector<std::string>{"~dev-libs/b-1"});
}

TEST_CASE("drift ignores inputs and meta") {
    CHECK(egraph::drift(decoded(egraph::test::assemble(egraph::test::sample_sections())),
                        decoded(with_section(1, Bytes{}.text("9").text("9").text("/x").varint(1))))
              .empty());
}

TEST_CASE("rebuild runs a full build") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    const auto invocation =
        command(store, fake_builder(dir.path(), fresh_sample(), 0), egraph::Rebuild{});
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
    CHECK(read_text(dir.path() / "args") ==
          std::format("--full --store {} --root /\n", store.string()));
    CHECK(std::filesystem::exists(store));
}

TEST_CASE("check passes when a fresh build matches the store") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    write_bytes(store, fresh_sample());
    const auto invocation =
        command(store, fake_builder(dir.path(), fresh_sample(), 0), egraph::Check{});
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::ok);
    CHECK(out.str().empty());
    CHECK(err.str().empty());
    // The fresh build went to a scratch file, which is gone again.
    const auto args = read_text(dir.path() / "args");
    CHECK(args.starts_with("--full --store "));
    const auto scratch = args.substr(15, args.find(' ', 15) - 15);
    CHECK(scratch != store.string());
    CHECK_FALSE(std::filesystem::exists(scratch));
}

TEST_CASE("check reports drift") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    write_bytes(store, fresh_sample());
    const auto invocation = command(store, fake_builder(dir.path(), only_b(3), 0), egraph::Check{});
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::drift);
    CHECK(out.str() == "-app-misc/a-1\n");
}

TEST_CASE("check fails without a store or a working builder") {
    const TempDir dir;
    const auto store = dir.path() / "installed.egraph";
    std::ostringstream out;
    std::ostringstream err;
    auto invocation = command(store, fake_builder(dir.path(), fresh_sample(), 0), egraph::Check{});
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::failure);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "args"));

    write_bytes(store, fresh_sample());
    invocation.builder = fake_builder(dir.path(), fresh_sample(), 9).string();
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::failure);
    CHECK(err.str().ends_with("exited with status 9\n"));
}
