#include "json.hpp"
#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string escaped(std::string_view bytes) {
    std::ostringstream out;
    egraph::write_json_string(out, bytes);
    return out.str();
}

} // namespace

// Expectations are Python's json.dumps(b.decode("utf-8", "surrogateescape")).
TEST_CASE("strings are escaped as Python's json module escapes them") {
    CHECK(escaped("plain") == R"("plain")");
    CHECK(escaped(R"(q"b\s/)") == R"("q\"b\\s/")");
    CHECK(escaped("\n\r\t\b\f\x01\x1f\x7f") == R"("\n\r\t\b\f\u0001\u001f\u007f")");
    CHECK(escaped("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == R"("\u00e9\u20ac\ud83d\ude00")");
    CHECK(escaped(std::string_view{"a\0b", 3}) == R"("a\u0000b")");
}

TEST_CASE("undecodable bytes are escaped one by one, as surrogateescape does") {
    CHECK(escaped("\xff") == R"("\udcff")");
    CHECK(escaped("a\xe2\x82Z") == R"("a\udce2\udc82Z")");
    // Encoded surrogates, overlongs and code points past U+10FFFF are not UTF-8.
    CHECK(escaped("\xed\xa0\x80") == R"("\udced\udca0\udc80")");
    CHECK(escaped("\xc0\xaf") == R"("\udcc0\udcaf")");
    CHECK(escaped("\xe0\x80\xaf") == R"("\udce0\udc80\udcaf")");
    CHECK(escaped("\xf4\x90\x80\x80") == R"("\udcf4\udc90\udc80\udc80")");
}

TEST_CASE("the sample store exports as the builder would") {
    const auto store = egraph::decode(egraph::test::assemble(egraph::test::sample_sections()));
    REQUIRE(store.has_value());
    std::ostringstream out;
    egraph::write_json(out, *store);
    CHECK(out.str() ==
          R"({"format":1,"packages":[)"
          R"({"cp":"app-misc/a","cpv":"app-misc/a-1","deps":{"BDEPEND":[],"DEPEND":[],)"
          R"("IDEPEND":[],"PDEPEND":[],"RDEPEND":[)"
          R"({"atom":"","matches":[],"parent":-1,"type":"any-of"},)"
          R"({"atom":"dev-libs/b","matches":["dev-libs/b-1"],"parent":0,"type":"atom"},)"
          R"({"atom":"dev-libs/missing","matches":[],"parent":0,"type":"atom"},)"
          R"({"atom":"!app-misc/old","matches":[],"parent":-1,"type":"weak-blocker"}]},)"
          R"("eapi":"8","errors":[["RDEPEND","bad dep"]],"iuse":["flag"],"provides":[],)"
          R"("repo":"test_repo","requires":[["x86_64","libb.so.1"]],"slot":"0","sub_slot":"0",)"
          R"("use":["flag"]},)"
          R"({"cp":"dev-libs/b","cpv":"dev-libs/b-1","deps":{"BDEPEND":[],"DEPEND":[],)"
          R"("IDEPEND":[],"PDEPEND":[],"RDEPEND":[]},"eapi":"8","errors":[],"iuse":[],)"
          R"("provides":[["x86_64","libb.so.1"]],"repo":"test_repo","requires":[],"slot":"0",)"
          R"("sub_slot":"0","use":[]}]})"
          "\n");
}
