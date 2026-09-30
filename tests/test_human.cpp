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

TEST_CASE("a verified plan says emerge agrees, or lists where it does not") {
    std::ostringstream same;
    egraph::human_verification(same, {}, plain);
    CHECK(same.str() == "\n+ emerge --pretend merges the same.\n");

    std::ostringstream differ;
    egraph::human_verification(differ,
                               std::vector<std::string>{
                                   "app-misc/kind-1::r\tkind\trebuild\tnew",
                                   "app-misc/only-ours-1::r\tegraph\tnew",
                                   "app-misc/only-theirs-3::r\temerge\tdowngrade",
                                   "dev-libs/use-1::r\tuse\tUSE=\"x -y\"\tUSE=\"-x -y\"",
                               },
                               plain);
    CHECK(differ.str() == "\n! emerge --pretend merges otherwise:\n"
                          "  app-misc/kind-1::r         rebuild here, new in emerge\n"
                          "  app-misc/only-ours-1::r    only here (new)\n"
                          "  app-misc/only-theirs-3::r  only in emerge (downgrade)\n"
                          "  dev-libs/use-1::r          USE=\"x -y\" here\n"
                          "                             USE=\"-x -y\" in emerge\n");
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

TEST_CASE("a held update's holders say what keeps them, and its remedies follow as commands") {
    std::ostringstream out;
    egraph::human_updates(
        out,
        std::vector<std::string>{
            "a/rgb-1\theld\ta/rgb-2\tgentoo\t\ta/skin-1 <a/rgb-2",
            "a/rgb-1\tholder\ta/skin-1\t\t@selected a/skin",
            "a/rgb-1\tremove\ta/effects-1 a/map-1",
            "a/rgb-1\tnodeps",
            "x/clc-22\theld\tx/clc-23\tgentoo\t\tx/mesa-1 =x/clc-22*\tx/pop-1 =x/clc-22*",
            "x/clc-22\tholder\tx/mesa-1\ta/kwin-1 a/qemu-1 a/gtk-4 a/sdl-2\t@system x/mesa",
            "x/clc-22\tholder\tx/pop-1\t",
            "x/clc-22\tnodeps",
            "x/own-1\theld\tx/own-2\tgentoo\t\tx/own-2 x/missing",
        },
        plain);
    CHECK(out.str() == "+ Nothing to update.\n"
                       "\nHeld back\n"
                       "H a/rgb  1  > 2   ::gentoo\n"
                       "    a/skin-1  <a/rgb-2\n"
                       "      nothing depends on it; only @selected keeps it\n"
                       "    to remove it: emerge --deselect a/skin\n"
                       "                  emerge -C =a/skin-1\n"
                       "                  emerge -1 =a/rgb-2\n"
                       "                  which also frees a/effects-1, a/map-1\n"
                       "    to keep it:   emerge -1 --nodeps =a/rgb-2\n"
                       "                  which a later emerge -uD undoes\n"
                       "H x/clc  22 > 23  ::gentoo\n"
                       "    x/mesa-1  =x/clc-22*\n"
                       "      needed by a/kwin-1, a/qemu-1, a/gtk-4 and 1 more; kept by @system\n"
                       "    x/pop-1   =x/clc-22*\n"
                       "      nothing depends on it or keeps it\n"
                       "    to keep them: emerge -1 --nodeps =x/clc-23\n"
                       "                  which a later emerge -uD undoes\n"
                       "H x/own  1  > 2   ::gentoo\n"
                       "    x/own-2  x/missing\n"
                       "\n3 held\n");
}

TEST_CASE("new packages come under their own heading, with what pulls them in") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "dev-cpp/glibmm-2.66\tupgrade\tdev-cpp/glibmm-2.66-r1\tgentoo",
                              "dev-cpp/mm-common-1.0.8\tnew\tdev-cpp/mm-common-1.0.8\tgentoo\t\t"
                              "dev-cpp/glibmm-2.66-r1 dev-cpp/mm-common",
                          },
                          plain);
    CHECK(out.str() == "U dev-cpp/glibmm     2.66  > 2.66-r1  ::gentoo\n"
                       "\nNew\n"
                       "N dev-cpp/mm-common  1.0.8            ::gentoo  "
                       "dev-cpp/glibmm-2.66-r1 dev-cpp/mm-common\n"
                       "\n1 upgrade, 1 new\n");
}

TEST_CASE("a new package's USE follows it on a line of its own") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "x/fresh-1\tnew\tx/fresh-1\tgentoo\t"
                              R"x(USE="a9 (fixed) -off (-stuck)" PYTHON_TARGETS="py3_13")x"
                              "\ta/grown-1 x/fresh",
                              "x/bare-1\tnew\tx/bare-1\tgentoo\tUSE=\"-doc\"",
                          },
                          plain);
    CHECK(out.str() == "\nNew\n"
                       "N x/fresh  1  ::gentoo  a/grown-1 x/fresh\n"
                       R"x(    USE="a9 (fixed) -off (-stuck)" PYTHON_TARGETS="py3_13")x"
                       "\n"
                       "N x/bare   1  ::gentoo\n"
                       "    USE=\"-doc\"\n"
                       "\n2 new\n"
                       "\n(flag) set by the profile\n");
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

TEST_CASE("the table numbers merges in order, with what each waits for") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "1\t\tdev-libs/chain-1\tnew\tdev-libs/chain-1\tgentoo\t\tdev-cpp/"
                              "mm-common-1 dev-libs/chain",
                              "2\t1\tdev-cpp/mm-common-1\tnew\tdev-cpp/mm-common-1\tgentoo\t"
                              "USE=\"doc\"\ta/glibmm-2 dev-cpp/mm-common",
                              "3\t2\ta/glibmm-1\tupgrade\ta/glibmm-2\tgentoo",
                              "\t\ta/host-1\theld\ta/host-2\tgentoo\t\ta/holder-1 <a/host-2",
                          },
                          plain, true);
    CHECK(out.str() ==
          "1 N dev-libs/chain     1      ::gentoo       dev-cpp/mm-common-1 dev-libs/chain\n"
          "2 N dev-cpp/mm-common  1      ::gentoo  w 1  a/glibmm-2 dev-cpp/mm-common\n"
          "      USE=\"doc\"\n"
          "3 U a/glibmm           1 > 2  ::gentoo  w 2\n"
          "\nHeld back\n"
          "  H a/host             1 > 2  ::gentoo\n"
          "      a/holder-1  <a/host-2\n"
          "\n1 upgrade, 2 new, 1 held\n");
}

TEST_CASE("the update tree hangs each merge from its root") {
    const std::vector<std::string> table{
        "1\t\ta/loose-1\tupgrade\ta/loose-2\tgentoo",
        "2\t\tx/chain-1\tnew\tx/chain-1\tgentoo\t\tx/mm-1 x/chain",
        "3\t2\tx/mm-1\tnew\tx/mm-1\tgentoo\tUSE=\"doc\"\ta/glibmm-2 x/mm",
        "4\t3\ta/glibmm-1\tupgrade\ta/glibmm-2\tgentoo",
    };
    const std::vector<std::string> tree{
        "1\t\ta/loose-1",
        "2\t@selected\ta/top-1\ta/glibmm-1\tx/mm-1\tx/chain-1",
        "3\t@selected\ta/top-1\ta/glibmm-1\tx/mm-1",
        "4\t@selected\ta/top-1\ta/glibmm-1",
    };
    std::ostringstream out;
    egraph::human_update_tree(out, table, tree, plain);
    CHECK(out.str() == "- nothing keeps\n"
                       "`- U a/loose  1 > 2  ::gentoo  1\n"
                       "@ @selected\n"
                       "`- a/top-1\n"
                       "   `- U a/glibmm  1 > 2  ::gentoo  4  w 3\n"
                       "      `- N x/mm  1  ::gentoo  3  w 2\n"
                       "         `- N x/chain  1  ::gentoo  2\n"
                       "\n2 upgrades, 2 new\n");
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

TEST_CASE("updates list uninstalls and blocks after the merges") {
    const std::vector<std::string> records{
        "a/user-1\tupgrade\ta/user-2\tgentoo",
        "a/new-1\tnew\ta/new-1\tgentoo",
        "a/old-1\tuninstall\ta/new-1\t!a/old\ta/old-1",
        "a/holder-1\tuninstall\ta/holder-1\t!a/new\ta/new-1",
        "a/new-1\tblocks\t!!a/kept\ta/kept-1",
    };
    std::ostringstream out;
    egraph::human_updates(out, records, plain);
    CHECK(out.str() == "U a/user  1 > 2  ::gentoo\n"
                       "\n"
                       "New\n"
                       "N a/new   1      ::gentoo\n"
                       "\n"
                       "Uninstalled\n"
                       "- a/old-1     blocked by a/new-1 !a/old\n"
                       "- a/holder-1  blocks a/new-1 !a/new\n"
                       "\n"
                       "Blocked\n"
                       "! a/new-1 !!a/kept  blocks a/kept-1\n"
                       "emerge refuses a plan with blockers it cannot resolve\n"
                       "\n"
                       "1 upgrade, 1 new, 2 uninstalls, 1 blocker\n");
}

TEST_CASE("blockers group by holder and count what they block") {
    const std::vector<std::string> records{
        "a/b-1\tRDEPEND\t!!x/gone\t",
        "a/b-1\tRDEPEND\t!x/old\tx/old-1",
        "c/d-2\tDEPEND\t!x/tool\tx/tool-1",
    };
    std::ostringstream out;
    egraph::human_blockers(out, records, true, plain);
    CHECK(out.str() == "* a/b-1\n"
                       "  R  !!x/gone  blocks nothing installed\n"
                       "  R  !x/old  blocks x/old-1\n"
                       "\n"
                       "* c/d-2\n"
                       "  D  !x/tool  blocks x/tool-1\n"
                       "\n"
                       "2 installed packages blocked\n" +
                           legend);
    std::ostringstream none;
    egraph::human_blockers(none, {}, false, plain);
    CHECK(none.str() == "+ No installed package blocks another.\n");
    std::ostringstream named;
    egraph::human_blockers(named, {}, true, plain);
    CHECK(named.str() == "+ None of them holds a blocker or is blocked.\n");
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
