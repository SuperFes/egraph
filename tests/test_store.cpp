#include "cli.hpp"
#include "store.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using egraph::NodeType;
using egraph::test::Bytes;
using egraph::test::Section;

namespace {

std::vector<std::byte> sample() {
    return egraph::test::assemble(egraph::test::sample_sections());
}

std::string rejection(const std::vector<std::byte>& bytes) {
    const auto store = egraph::decode(bytes);
    REQUIRE_FALSE(store.has_value());
    return store.error().message;
}

// The sample with one section replaced.
std::vector<std::byte> with_section(std::uint64_t id, const Bytes& bytes) {
    auto sections = egraph::test::sample_sections();
    for (auto& section : sections) {
        if (section.id == id) {
            section.bytes = bytes.bytes();
        }
    }
    return egraph::test::assemble(sections);
}

// A packages section holding one package whose RDEPEND is written by nodes.
std::vector<std::byte> with_rdepend(std::uint64_t count, const std::function<void(Bytes&)>& nodes) {
    Bytes packages;
    packages.varint(1).varints({1, 2, 3, 3, 4, 5}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(count);
    nodes(packages);
    packages.varint(0).varint(0);
    return with_section(4, packages);
}

class TempFile {
  public:
    explicit TempFile(const std::vector<std::byte>& bytes)
        : path_(std::filesystem::temp_directory_path() /
                ("egraph-test-" + std::to_string(counter++) + ".egraph")) {
        std::ofstream out(path_, std::ios::binary);
        for (const auto byte : bytes) {
            out.put(static_cast<char>(byte));
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    static inline int counter = 0;
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("the sample store decodes") {
    const auto store = egraph::decode(sample());
    REQUIRE(store.has_value());
    CHECK(store->meta.egraph_version == "0.0.0");
    CHECK(store->meta.portage_version == "3.0.0");
    CHECK(store->meta.eroot == "/");
    CHECK(store->meta.build_time_ns == 42);
    REQUIRE(store->inputs.size() == 1);
    CHECK(store->inputs.front().path == "/var/db/pkg/app-misc");
    CHECK(store->inputs.front().kind == egraph::InputKind::directory);
    CHECK(store->inputs.front().mtime_ns == 5);

    REQUIRE(store->packages.size() == 2);
    const auto& a = store->packages.front();
    CHECK(store->string(a.cpv) == "app-misc/a-1");
    CHECK(store->string(a.cp) == "app-misc/a");
    CHECK(store->ids_in(a.use).size() == 1);
    REQUIRE(store->pairs_in(a.errors).size() == 1);
    CHECK(store->string(store->pairs_in(a.errors).front().second) == "bad dep");

    const auto nodes = store->nodes_in(a.deps.at(4));
    const std::vector<egraph::Node> rdepend(nodes.begin(), nodes.end());
    REQUIRE(rdepend.size() == 4);
    CHECK(rdepend.at(0).type == NodeType::any_of);
    CHECK(rdepend.at(0).parent == egraph::no_parent);
    CHECK(rdepend.at(1).parent == 0);
    CHECK(store->string(rdepend.at(1).atom) == "dev-libs/b");
    REQUIRE(store->ids_in(rdepend.at(1).matches).size() == 1);
    CHECK(store->ids_in(rdepend.at(1).matches).front() == 1);
    CHECK(store->ids_in(rdepend.at(2).matches).empty());
    CHECK(rdepend.at(3).type == NodeType::weak_blocker);
    for (std::size_t kind = 0; kind < 4; ++kind) {
        CHECK(store->nodes_in(a.deps.at(kind)).empty());
    }

    REQUIRE(store->required_in(a.required).size() == 1);
    const auto& require = store->required_in(a.required).front();
    CHECK(store->string(require.soname) == "libb.so.1");
    CHECK(store->ids_in(require.providers).size() == 1);
    CHECK(store->pairs_in(store->packages.back().provided).size() == 1);
}

TEST_CASE("every truncation is rejected") {
    const auto bytes = sample();
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const std::vector<std::byte> prefix(bytes.begin(),
                                            bytes.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK_FALSE(egraph::decode(prefix).has_value());
    }
}

// Run under ASan+UBSan: whatever the bytes, decode returns rather than crashing.
TEST_CASE("corrupted bytes never crash the decoder") {
    const auto bytes = sample();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (const std::byte value :
             {std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x80},
              bytes.at(i) ^ std::byte{0x01}, bytes.at(i) ^ std::byte{0x40}}) {
            auto corrupt = bytes;
            corrupt.at(i) = value;
            const auto store = egraph::decode(corrupt);
            CHECK((store.has_value() || !store.error().message.empty()));
        }
    }
}

TEST_CASE("the header is checked") {
    auto bytes = sample();
    bytes.at(0) = std::byte{'X'};
    CHECK(rejection(bytes) == "not an egraph store");

    CHECK(rejection(egraph::test::assemble(egraph::test::sample_sections(), 2)) ==
          "format version 2, expected 1");

    auto sections = egraph::test::sample_sections();
    sections.pop_back();
    CHECK(rejection(egraph::test::assemble(sections)) == "bad section table");

    sections = egraph::test::sample_sections();
    sections.back().id = 1;
    CHECK(rejection(egraph::test::assemble(sections)) == "unexpected section 1");

    sections = egraph::test::sample_sections();
    sections.back().id = 6;
    CHECK(rejection(egraph::test::assemble(sections)) == "unexpected section 6");
}

TEST_CASE("a section must lie inside the file") {
    auto bytes = sample();
    // The last entry's length, one byte past the end.
    const std::size_t length_at = 16 + (20 * 4) + 12;
    bytes.at(length_at) = std::byte{2};
    CHECK(rejection(bytes) == "section 5 outside the file");
}

TEST_CASE("sections are checked") {
    CHECK_THAT(rejection(with_section(5, Bytes{}.varint(0).varint(0))),
               Catch::Matchers::StartsWith("roots: trailing bytes"));
    CHECK_THAT(rejection(with_section(5, Bytes{}.varint(9))),
               Catch::Matchers::StartsWith("roots: count 9 exceeds the bytes left"));
    CHECK_THAT(rejection(with_section(3, Bytes{}.varint(1).text("x"))),
               Catch::Matchers::StartsWith("strings: string 0 must be empty"));
    CHECK_THAT(
        rejection(with_section(2, Bytes{}.varint(1).text("/").varint(2).varint(0).varint(0))),
        Catch::Matchers::StartsWith("inputs: input kind 2 out of range 2"));
    // Nine continuation bytes leave one bit for the tenth.
    Bytes overflow;
    for (int i = 0; i < 9; ++i) {
        overflow.fixed(0x80, 1);
    }
    overflow.fixed(0x02, 1);
    CHECK_THAT(rejection(with_section(1, overflow)),
               Catch::Matchers::StartsWith("meta: varint exceeds 64 bits"));
}

TEST_CASE("dependency trees are checked") {
    // Type, parent, atom, matches.
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({5, 0, 7}).list({}); })),
               Catch::Matchers::StartsWith("packages: node type 5 out of range 5"));
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({0, 0, 7}).list({1}); })),
               Catch::Matchers::StartsWith("packages: package 1 out of range 1"));
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({0, 1, 7}).list({}); })),
               Catch::Matchers::StartsWith("packages: parent 1 out of range 1"));
    CHECK_THAT(rejection(with_rdepend(2,
                                      [](Bytes& b) {
                                          b.varints({0, 0, 7}).list({});
                                          b.varints({0, 1, 14}).list({});
                                      })),
               Catch::Matchers::StartsWith("packages: parent is not a group"));
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({1, 0, 7}).list({}); })),
               Catch::Matchers::StartsWith("packages: only groups have no atom"));
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({0, 0, 0}).list({}); })),
               Catch::Matchers::StartsWith("packages: only groups have no atom"));
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({2, 0, 0}).list({0}); })),
               Catch::Matchers::StartsWith("packages: a group has matches"));
    CHECK_THAT(rejection(with_rdepend(1, [](Bytes& b) { b.varints({0, 0, 99}).list({}); })),
               Catch::Matchers::StartsWith("packages: string 99 out of range 15"));
}

TEST_CASE("load names the file in its errors") {
    const TempFile file(with_section(5, Bytes{}.varint(0).varint(0)));
    const auto store = egraph::load(file.path());
    REQUIRE_FALSE(store.has_value());
    CHECK_THAT(store.error().message,
               Catch::Matchers::StartsWith(file.path().string() + ": roots: trailing bytes"));

    const auto missing = egraph::load("/nonexistent/egraph.store");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message == "/nonexistent/egraph.store: No such file or directory");
}

TEST_CASE("the default store lives under the root") {
    CHECK(egraph::default_store_path("/mnt/target") ==
          std::filesystem::path{"/mnt/target/var/cache/egraph/installed.egraph"});
}

TEST_CASE("export --format json reads the store") {
    const TempFile file(sample());
    egraph::Invocation invocation;
    invocation.store = file.path();
    invocation.command = egraph::Export{.format = egraph::ExportFormat::json, .packages = {}};
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(egraph::run(invocation, out, err) == egraph::Exit::ok);
    CHECK(err.str().empty());
    CHECK_THAT(out.str(), Catch::Matchers::StartsWith(R"({"format":1,"packages":[{"cp":)"));

    invocation.store = "/nonexistent/egraph.store";
    std::ostringstream missing;
    CHECK(egraph::run(invocation, out, missing) == egraph::Exit::failure);
    CHECK(missing.str() == "egraph: /nonexistent/egraph.store: No such file or directory\n");
}

TEST_CASE("export of dot or a neighborhood is not implemented yet") {
    egraph::Invocation invocation;
    invocation.command = egraph::Export{.format = egraph::ExportFormat::dot, .packages = {}};
    std::ostringstream out;
    std::ostringstream err;
    CHECK(egraph::run(invocation, out, err) == egraph::Exit::not_implemented);
}
