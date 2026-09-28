#include "human.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

namespace {

const egraph::Painter plain{false};

} // namespace

TEST_CASE("colour wraps text in its tone's style") {
    const egraph::Painter color{true};
    CHECK(color("dev-libs/a-1", egraph::Tone::package) == "\x1b[1mdev-libs/a-1\x1b[0m");
    CHECK(color("RDEPEND", egraph::Tone::kind) == "\x1b[36mRDEPEND\x1b[0m");
    CHECK(color("", egraph::Tone::bad).empty());
    CHECK(plain("dev-libs/a-1", egraph::Tone::package) == "dev-libs/a-1");
}

TEST_CASE("deps list each dependency once, with every kind it appears under") {
    const std::vector<std::string> records{
        "app-misc/a-1\tBDEPEND\tdev-util/tool\tdev-util/tool-2",
        "app-misc/a-1\tDEPEND\tdev-libs/b\tdev-libs/b-1",
        "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1",
        "app-misc/a-1\tRDEPEND\t|| alt\tdev-libs/c-10\tany-of",
    };
    const std::vector<std::string> subjects{"app-misc/a-1", "app-misc/z-1"};
    std::ostringstream out;
    egraph::human_edges(out, records, subjects, false, plain);
    CHECK(out.str() == "app-misc/a-1\n"
                       "  dev-libs/b-1     RDEPEND DEPEND  dev-libs/b\n"
                       "  dev-libs/c-10    RDEPEND         || alt  any-of\n"
                       "  dev-util/tool-2  BDEPEND         dev-util/tool\n"
                       "\n"
                       "app-misc/z-1\n"
                       "  no dependencies\n");
}

TEST_CASE("rdeps group by the package depended on") {
    const std::vector<std::string> records{"app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1"};
    const std::vector<std::string> subjects{"dev-libs/b-1", "app-misc/a-1"};
    std::ostringstream out;
    egraph::human_edges(out, records, subjects, true, plain);
    CHECK(out.str() == "dev-libs/b-1\n"
                       "  app-misc/a-1  RDEPEND  dev-libs/b\n"
                       "\n"
                       "app-misc/a-1\n"
                       "  nothing installed depends on it\n");
}

TEST_CASE("why draws the chain from its root") {
    const std::vector<std::string> records{
        "@selected\tapp-misc/a\tapp-misc/a-1",
        "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1",
        "dev-libs/b-1\tPDEPEND\t>=dev-libs/c-2\tdev-libs/c-2\tany-of",
    };
    std::ostringstream out;
    egraph::human_path(out, records, plain);
    CHECK(out.str() == "@selected  app-misc/a\n"
                       "└─ app-misc/a-1\n"
                       "   └─ dev-libs/b-1     RDEPEND dev-libs/b\n"
                       "      └─ dev-libs/c-2  PDEPEND >=dev-libs/c-2  any-of\n");
}

TEST_CASE("orphans end with a count") {
    std::ostringstream out;
    egraph::human_orphans(out, std::vector<std::string>{"a/b-1"}, plain);
    CHECK(out.str() == "a/b-1\n\n1 package depclean would remove\n");
    std::ostringstream none;
    egraph::human_orphans(none, {}, plain);
    CHECK(none.str() == "Nothing to remove.\n");
}

TEST_CASE("broken groups by package and counts") {
    const std::vector<std::string> records{
        "a/b-1\tPDEPEND\tx/gone",
        "a/b-1\tRDEPEND\t|| ( x/y x/z )",
        "c/d-2\tBDEPEND\tx/old",
    };
    std::ostringstream out;
    egraph::human_broken(out, records, plain);
    CHECK(out.str() == "a/b-1\n"
                       "  PDEPEND  x/gone\n"
                       "  RDEPEND  || ( x/y x/z )\n"
                       "\n"
                       "c/d-2\n"
                       "  BDEPEND  x/old\n"
                       "\n"
                       "3 unsatisfied dependencies in 2 packages\n");
    std::ostringstream none;
    egraph::human_broken(none, {}, plain);
    CHECK(none.str() == "Every dependency is satisfied.\n");
}

TEST_CASE("soname users group by multilib category") {
    const std::vector<std::string> records{"a/b-1\tx86_64", "c/d-1\tx86_32", "e/f-1\tx86_64"};
    std::ostringstream out;
    egraph::human_soname(out, records, "libz.so.1", false, plain);
    CHECK(out.str() == "libz.so.1 is used by\n"
                       "  x86_32\n"
                       "    c/d-1\n"
                       "  x86_64\n"
                       "    a/b-1\n"
                       "    e/f-1\n");
    std::ostringstream none;
    egraph::human_soname(none, {}, "libq.so", true, plain);
    CHECK(none.str() == "Nothing installed provides libq.so.\n");
}

TEST_CASE("match lists each atom's packages") {
    const std::vector<std::string> atoms{"x/a", ">=x/b-2"};
    std::ostringstream out;
    egraph::human_match(out, std::vector<std::string>{"x/a\tx/a-1", "x/a\tx/a-2"}, atoms, plain);
    CHECK(out.str() == "x/a\n  x/a-1\n  x/a-2\n\n>=x/b-2\n  no installed package\n");
}
