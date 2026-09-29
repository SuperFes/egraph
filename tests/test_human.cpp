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

TEST_CASE("possible dependencies show the USE that would add them") {
    const std::vector<std::string> records{
        "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1",
        "app-misc/a-1\tRDEPEND\tdev-libs/b[doc]\tdev-libs/b-1\tuse=doc",
        "app-misc/a-1\tRDEPEND\tdev-libs/c\tdev-libs/c-1\tany-of\tuse=a -minimal",
        "app-misc/a-1\tDEPEND\tdev-libs/c\tdev-libs/c-1\tany-of\tuse=a -minimal",
    };
    const std::vector<std::string> subjects{"app-misc/a-1"};
    std::ostringstream out;
    egraph::human_edges(out, records, subjects, false, plain);
    CHECK(out.str() == "* app-misc/a-1  3 dependencies\n"
                       "  R....  dev-libs/b-1  dev-libs/b\n"
                       "  R....  dev-libs/b-1  dev-libs/b[doc]  +doc\n"
                       "  R..D.  dev-libs/c-1  dev-libs/c |  +a -minimal\n" +
                           legend +
                           "+flag -flag USE the ebuild would need to add a possible "
                           "dependency\n");
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

TEST_CASE("updates line up versions and repositories, and count each kind") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "app-misc/up-1\tupgrade\tapp-misc/up-10\ttest_repo",
                              "app-misc/down-2\tdowngrade\tapp-misc/down-1\toverlay",
                              "app-misc/use-1\trebuild\tapp-misc/use-1\ttest_repo\ta%* -b%",
                          },
                          plain);
    CHECK(out.str() == "U app-misc/up    1 > 10  ::test_repo\n"
                       "D app-misc/down  2 > 1   ::overlay\n"
                       "R app-misc/use   1       ::test_repo  a%* -b%\n"
                       "\n1 upgrade, 1 downgrade, 1 rebuild\n"
                       "\nflag* changed  flag% new in IUSE  (-flag%) gone from it\n");
    std::ostringstream two;
    egraph::human_updates(
        two, std::vector<std::string>{"a/b-1\tupgrade\ta/b-2\tr", "a/c-1\tupgrade\ta/c-2\tr"},
        plain);
    CHECK(two.str() == "U a/b  1 > 2  ::r\nU a/c  1 > 2  ::r\n\n2 upgrades\n");
    std::ostringstream none;
    egraph::human_updates(none, {}, plain);
    CHECK(none.str() == "+ Nothing to update.\n");
}

TEST_CASE("held updates come once each, their holders under them") {
    std::ostringstream out;
    egraph::human_updates(
        out,
        std::vector<std::string>{
            "app-misc/up-1\tupgrade\tapp-misc/up-10\ttest_repo",
            "app-misc/gcr-3\theld\tapp-misc/gcr-3\tgentoo\t-gtk*\tgnome/keyring-50 "
            ">=app-misc/gcr-3:0=[gtk] >=app-misc/gcr-3:0/1=[gtk]",
            "app-misc/rgb-1\theld\tapp-misc/rgb-2\tgentoo\t\tapp-misc/skin-1 "
            "<app-misc/rgb-2\tapp-misc/effects-1 <app-misc/rgb-2",
        },
        plain);
    CHECK(out.str() ==
          "U app-misc/up   1 > 10  ::test_repo\n"
          "\nHeld back\n"
          "H app-misc/gcr  3       ::gentoo  -gtk*\n"
          "    gnome/keyring-50  >=app-misc/gcr-3:0=[gtk]  >=app-misc/gcr-3:0/1=[gtk]\n"
          "H app-misc/rgb  1 > 2   ::gentoo\n"
          "    app-misc/skin-1     <app-misc/rgb-2\n"
          "    app-misc/effects-1  <app-misc/rgb-2\n"
          "\n1 upgrade, 2 held\n"
          "\nflag* changed  flag% new in IUSE  (-flag%) gone from it\n");
    std::ostringstream only;
    egraph::human_updates(only, std::vector<std::string>{"a/b-1\theld\ta/b-2\tr\t\ta/c-1 <a/b-2"},
                          plain);
    CHECK(only.str() == "+ Nothing to update.\n"
                        "\nHeld back\n"
                        "H a/b  1 > 2  ::r\n"
                        "    a/c-1  <a/b-2\n"
                        "\n1 held\n");
}

TEST_CASE("new packages come under their own heading, with what pulls them in") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "dev-cpp/glibmm-2.66\tupgrade\tdev-cpp/glibmm-2.66-r1\tgentoo",
                              "dev-cpp/mm-common-1.0.8\tnew\tdev-cpp/mm-common-1.0.8\tgentoo\t"
                              "dev-cpp/glibmm-2.66-r1 dev-cpp/mm-common",
                          },
                          plain);
    CHECK(out.str() == "U dev-cpp/glibmm     2.66  > 2.66-r1  ::gentoo\n"
                       "\nNew\n"
                       "N dev-cpp/mm-common  1.0.8            ::gentoo  "
                       "dev-cpp/glibmm-2.66-r1 dev-cpp/mm-common\n"
                       "\n1 upgrade, 1 new\n");
}

TEST_CASE("a slot-operator rebuild says which merge it is for") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "a/kwin-1\trebuild\ta/kwin-1\tgentoo\t\tx/lib-2 x/lib:0/1=",
                              "x/lib-1\tupgrade\tx/lib-2\tgentoo",
                          },
                          plain);
    CHECK(out.str() == "R a/kwin  1      ::gentoo  x/lib-2 x/lib:0/1=\n"
                       "U x/lib   1 > 2  ::gentoo\n"
                       "\n1 upgrade, 1 rebuild\n");
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
        for (const auto text : {glyph.package,
                                glyph.selected,
                                glyph.system,
                                glyph.profile,
                                glyph.set,
                                glyph.orphan,
                                glyph.broken,
                                glyph.soname,
                                glyph.search,
                                glyph.good,
                                glyph.choice,
                                glyph.branch,
                                glyph.absent,
                                glyph.tee,
                                glyph.rail,
                                glyph.folded,
                                glyph.unfolded,
                                glyph.cycle,
                                glyph.instead,
                                glyph.upgrade,
                                glyph.downgrade,
                                glyph.rebuild,
                                glyph.frame.top_left,
                                glyph.frame.top_right,
                                glyph.frame.bottom_left,
                                glyph.frame.bottom_right,
                                glyph.frame.across,
                                glyph.frame.down,
                                glyph.build,
                                glyph.binary,
                                glyph.merge,
                                glyph.waiting,
                                glyph.queued,
                                glyph.spinner,
                                glyph.bar_full,
                                glyph.bar_empty,
                                glyph.spark}) {
            CHECK_FALSE(text.empty());
        }
    }
}
