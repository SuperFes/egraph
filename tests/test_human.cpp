#include "human.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

using egraph::ColorDepth;
using egraph::Tone;

namespace {

const egraph::Theme plain{.paint = egraph::Painter{ColorDepth::none},
                          .glyph_set = egraph::GlyphSet::ascii};

const std::string legend = "\nR runtime  I install  P post  D build  B build host  "
                           "| one alternative of a || group\n";

} // namespace

TEST_CASE("colour wraps text in its tone's style, at the terminal's depth") {
    const egraph::Painter truecolor{ColorDepth::truecolor};
    const egraph::Painter palette{ColorDepth::palette};
    CHECK(truecolor("x", Tone::version) == "\x1b[38;2;166;227;161mx\x1b[0m");
    CHECK(palette("x", Tone::version) == "\x1b[38;5;150mx\x1b[0m");
    CHECK(palette("x", Tone::name) == "\x1b[1;38;5;189mx\x1b[0m");
    CHECK(palette("x", Tone::note) == "\x1b[3;38;5;245mx\x1b[0m");
    CHECK(palette("", Tone::bad).empty());
    CHECK(plain.paint("dev-libs/a-1", Tone::name) == "dev-libs/a-1");
}

TEST_CASE("cpvs and atoms are coloured part by part") {
    // Tones as single letters, to read the split.
    const egraph::Painter marks{ColorDepth::palette};
    const auto strip = [](std::string text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text.at(i) == '\x1b') {
                const auto end = text.find('m', i);
                out += text.substr(i, end - i + 1) == "\x1b[0m" ? "}" : "{";
                i = end;
            } else {
                out += text.at(i);
            }
        }
        return out;
    };
    CHECK(strip(egraph::paint_cpv("dev-libs/foo-bar-1.2-r3", marks)) ==
          "{dev-libs}{/}{foo-bar}{-}{1.2-r3}");
    CHECK(strip(egraph::paint_cpv("dev-libs/foo", marks)) == "{dev-libs}{/}{foo}");
    CHECK(strip(egraph::paint_dependency(">=dev-libs/a-2:0/3=::gentoo[x,-y(+)]", marks)) ==
          "{>=}{dev-libs}{/}{a}{-}{2}{:0/3=}{::gentoo}{[}{x}{,}{-y(+)}{]}");
    CHECK(strip(egraph::paint_dependency("|| ( a/b =a/c-1* )", marks)) ==
          "{||} {(} {a}{/}{b} {=}{a}{/}{c}{-}{1}{*} {)}");
}

TEST_CASE("deps show each dependency once, its kinds as a letter matrix") {
    const std::vector<std::string> records{
        "app-misc/a-1\tBDEPEND\tdev-util/tool\tdev-util/tool-2",
        "app-misc/a-1\tDEPEND\tdev-libs/b\tdev-libs/b-1",
        "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1",
        "app-misc/a-1\tRDEPEND\tdev-libs/c\tdev-libs/c-10\tany-of",
    };
    const std::vector<std::string> subjects{"app-misc/a-1", "app-misc/z-1"};
    std::ostringstream out;
    egraph::human_edges(out, records, subjects, false, plain);
    CHECK(out.str() == "* app-misc/a-1  3 dependencies\n"
                       "  R..D.  dev-libs/b-1     dev-libs/b\n"
                       "  R....  dev-libs/c-10    dev-libs/c |\n"
                       "  ....B  dev-util/tool-2  dev-util/tool\n"
                       "\n"
                       "* app-misc/z-1  0 dependencies\n" +
                           legend);
}

TEST_CASE("rdeps group by the package depended on") {
    const std::vector<std::string> records{"app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1"};
    const std::vector<std::string> subjects{"dev-libs/b-1"};
    std::ostringstream out;
    egraph::human_edges(out, records, subjects, true, plain);
    CHECK(out.str() == "* dev-libs/b-1  1 dependent\n"
                       "  R....  app-misc/a-1  dev-libs/b\n" +
                           legend);
}

TEST_CASE("why draws the chain from its root") {
    const std::vector<std::string> records{
        "@selected\tapp-misc/a\tapp-misc/a-1",
        "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1",
        "dev-libs/b-1\tPDEPEND\t>=dev-libs/c-2\tdev-libs/c-2\tany-of",
    };
    std::ostringstream out;
    egraph::human_path(out, records, plain);
    CHECK(out.str() == "* dev-libs/c-2  is kept by\n"
                       "@ @selected  app-misc/a\n"
                       "`- app-misc/a-1\n"
                       "   `- dev-libs/b-1     R  dev-libs/b\n"
                       "      `- dev-libs/c-2  P  >=dev-libs/c-2 |\n");
}

TEST_CASE("orphans end with a count") {
    std::ostringstream out;
    egraph::human_orphans(out, std::vector<std::string>{"a/b-1"}, plain);
    CHECK(out.str() == "- a/b-1\n\n1 package depclean would remove\n");
    std::ostringstream none;
    egraph::human_orphans(none, {}, plain);
    CHECK(none.str() == "+ Nothing to remove.\n");
}

TEST_CASE("broken groups by package and counts") {
    const std::vector<std::string> records{
        "a/b-1\tPDEPEND\tx/gone",
        "a/b-1\tRDEPEND\t|| ( x/y x/z )\tx/y-0.9",
        "c/d-2\tBDEPEND\tx/old",
    };
    std::ostringstream out;
    egraph::human_broken(out, records, {}, plain);
    CHECK(out.str() == "! a/b-1\n"
                       "  P  x/gone\n"
                       "  R  || ( x/y x/z )  > x/y-0.9\n"
                       "\n"
                       "! c/d-2\n"
                       "  B  x/old\n"
                       "\n"
                       "3 unsatisfied dependencies in 2 packages\n" +
                           legend);
    std::ostringstream none;
    egraph::human_broken(none, {}, {}, plain);
    CHECK(none.str() == "+ Every dependency is satisfied.\n");
}

TEST_CASE("broken lists replaced build-time dependencies apart, and not as breakage") {
    const std::vector<std::string> replaced{
        "a/long-name-1\tBDEPEND\t>=x/automake-1.18:1.18\tx/automake-1.19",
        "c/d-2\tDEPEND\t|| ( x/p:1 x/q:1 )\tx/p-2 x/q-2",
    };
    std::ostringstream out;
    egraph::human_broken(out, {}, replaced, plain);
    CHECK(out.str() == "+ Nothing is broken.\n"
                       "\n"
                       "Built with, since replaced  2\n"
                       "  a/long-name-1  B  >=x/automake-1.18:1.18  > x/automake-1.19\n"
                       "  c/d-2          D  || ( x/p:1 x/q:1 )  > x/p-2 x/q-2\n" +
                           legend);
}

TEST_CASE("soname users group by multilib category") {
    const std::vector<std::string> records{"a/b-1\tx86_64", "c/d-1\tx86_32", "e/f-1\tx86_64"};
    std::ostringstream out;
    egraph::human_soname(out, records, "libz.so.1", false, plain);
    CHECK(out.str() == "~ libz.so.1  used by 3 packages\n"
                       "  x86_32\n"
                       "    * c/d-1\n"
                       "  x86_64\n"
                       "    * a/b-1\n"
                       "    * e/f-1\n");
    std::ostringstream none;
    egraph::human_soname(none, {}, "libq.so", true, plain);
    CHECK(none.str() == "~ libq.so  nothing installed provides it\n");
}

TEST_CASE("match lists each atom's packages") {
    const std::vector<std::string> atoms{"x/a", ">=x/b-2"};
    std::ostringstream out;
    egraph::human_match(out, std::vector<std::string>{"x/a\tx/a-1", "x/a\tx/a-2"}, atoms, plain);
    CHECK(out.str() ==
          "? x/a  2 matches\n  * x/a-1\n  * x/a-2\n\n? >=x/b-2  no installed package\n");
}

TEST_CASE("every glyph set fills every glyph") {
    for (const auto set :
         {egraph::GlyphSet::nerd, egraph::GlyphSet::unicode, egraph::GlyphSet::ascii}) {
        const auto& glyph = egraph::glyphs(set);
        for (const auto text :
             {glyph.package, glyph.selected, glyph.system, glyph.profile, glyph.set, glyph.orphan,
              glyph.broken, glyph.soname, glyph.search, glyph.good, glyph.choice, glyph.branch,
              glyph.absent, glyph.tee, glyph.rail, glyph.folded, glyph.unfolded, glyph.cycle,
              glyph.instead}) {
            CHECK_FALSE(text.empty());
        }
    }
}
