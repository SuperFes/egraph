#include "history.hpp"
#include "human.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
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

TEST_CASE("a removal lists what goes, then what is kept and why") {
    std::ostringstream out;
    egraph::human_removal(out,
                          std::vector<std::string>{"a/b-1\tremove", "a/lib-1\tkept\ta/user-1",
                                                   "a/base-1\tkept\t@system"},
                          plain);
    CHECK(out.str() == "- a/b-1\n"
                       "H a/lib-1   needed by a/user-1\n"
                       "H a/base-1  kept by @system\n"
                       "\n1 package to remove, 2 kept\n");
    std::ostringstream kept;
    egraph::human_removal(kept, std::vector<std::string>{"a/lib-1\tkept\ta/user-1"}, plain);
    CHECK(kept.str() == "H a/lib-1  needed by a/user-1\n\nNothing to remove, 1 kept\n");
}

TEST_CASE("a deselect lists the atoms leaving, then what depclean would remove after") {
    std::ostringstream out;
    egraph::human_deselect(
        out, std::vector<std::string>{"a/user\tdeselect", "@myset\tdeselect", "a/user-1\torphan"},
        plain);
    CHECK(out.str() == "- a/user\n- @myset\n\n2 atoms leave @selected; then emerge --depclean "
                       "would remove:\n- a/user-1\n");
    std::ostringstream alone;
    egraph::human_deselect(alone, std::vector<std::string>{"a/leaf\tdeselect"}, plain);
    CHECK(alone.str() == "- a/leaf\n\n1 atom leaves @selected\n");
}

TEST_CASE("an action's changes to @selected follow it") {
    std::ostringstream none;
    egraph::human_selection(none, {}, plain);
    CHECK(none.str().empty());
    std::ostringstream out;
    egraph::human_selection(out, std::vector<std::string>{"a/b\tdeselected", "c/d\tselected"},
                            plain);
    CHECK(out.str() == "\n+ c/d joined @selected\n- a/b left @selected\n");
}

TEST_CASE("elog messages follow by package, each class and phase once") {
    std::ostringstream none;
    egraph::human_elog(none, {}, plain);
    CHECK(none.str().empty());
    std::ostringstream out;
    egraph::human_elog(out,
                       std::vector<std::string>{"a/b-1\telog\tLOG\tpostinst\tFirst.",
                                                "a/b-1\telog\tLOG\tpostinst\t",
                                                "a/b-1\telog\tLOG\tpostinst\tSecond.",
                                                "a/b-1\telog\tWARN\tpostinst\tCareful.",
                                                "c/d-2\telog\tQA\tother\tQA Notice: x"},
                       plain);
    CHECK(out.str() == "\nMessages for a/b-1:\n  postinst (LOG)\n    First.\n\n    Second.\n"
                       "  postinst (WARN)\n    Careful.\n"
                       "\nMessages for c/d-2:\n  other (QA)\n    QA Notice: x\n");
}

TEST_CASE("notices list configuration updates by file, then unread news") {
    std::ostringstream none;
    egraph::human_notices(none, {}, plain, egraph::Seconds{});
    CHECK(none.str().empty());
    std::ostringstream out;
    egraph::human_notices(out,
                          std::vector<std::string>{"/etc/a\tconfig\t/etc/._cfg0000_a",
                                                   "/etc/a\tconfig\t/etc/._cfg0001_a",
                                                   "/etc/b\tconfig\t/etc/._cfg0000_b",
                                                   "2026-09-01-x\tnews\tgentoo\tX happened"},
                          plain, egraph::Seconds{});
    CHECK(out.str() == "Configuration updates (dispatch-conf):\n  /etc/a  2 updates\n  /etc/b\n"
                       "\nUnread news (eselect news read):\n  2026-09-01-x  X happened\n");
    std::ostringstream news;
    egraph::human_notices(news, std::vector<std::string>{"2026-09-01-x\tnews\tgentoo\t"}, plain,
                          egraph::Seconds{});
    CHECK(news.str() == "Unread news (eselect news read):\n  2026-09-01-x\n");
}

TEST_CASE("notices list what a configuration edit did to the plan after its updates") {
    std::ostringstream out;
    egraph::human_notices(out,
                          std::vector<std::string>{"/etc/a\tconfig\t/etc/._cfg0000_a",
                                                   "title\tplan\tConfiguration edit: +1 rebuild",
                                                   "detail\tplan\tedited /etc/portage/package.use",
                                                   "detail\tplan\t+ dev-libs/d-1 rebuild"},
                          plain, egraph::Seconds{});
    CHECK(out.str() == "Configuration updates (dispatch-conf):\n  /etc/a\n"
                       "\nConfiguration edit: +1 rebuild (egraph updates):\n"
                       "  edited /etc/portage/package.use\n  + dev-libs/d-1 rebuild\n");
}

TEST_CASE("notices list the configuration check's findings by file after the plan's change") {
    std::ostringstream out;
    egraph::human_notices(out,
                          std::vector<std::string>{"title\tplan\tConfiguration edit: +1 rebuild",
                                                   "/etc/portage/package.mask\tcheck\t0\t1\t0",
                                                   "/etc/portage/package.use\tcheck\t1\t2\t1",
                                                   "2026-09-01-x\tnews\tgentoo\tX happened"},
                          plain, egraph::Seconds{});
    CHECK(out.str() == "Configuration edit: +1 rebuild (egraph updates):\n"
                       "\nConfiguration check (egraph config check):\n"
                       "  /etc/portage/package.mask  1 warning\n"
                       "  /etc/portage/package.use  1 error, 2 warnings, 1 note\n"
                       "\nUnread news (eselect news read):\n  2026-09-01-x  X happened\n");
}

TEST_CASE("notices list preserved libraries with what they come from and what uses them") {
    std::ostringstream out;
    egraph::human_notices(
        out,
        std::vector<std::string>{"2026-09-01-x\tnews\tgentoo\tX happened",
                                 "/usr/lib/libfoo.so.1\tpreserved\tdev-libs/foo-2\ta/bar-1 a/baz-1",
                                 "/usr/lib/libold.so.1\tpreserved\tdev-libs/old-2\t",
                                 "a/bar:0\trebuild", "a/baz:0\trebuild"},
        plain, egraph::Seconds{});
    CHECK(out.str() == "Unread news (eselect news read):\n  2026-09-01-x  X happened\n"
                       "\nPreserved libraries (egraph install -1 @preserved-rebuild):\n"
                       "  /usr/lib/libfoo.so.1  from dev-libs/foo-2, used by a/bar-1, a/baz-1\n"
                       "  /usr/lib/libold.so.1  from dev-libs/old-2\n");
}

TEST_CASE("notices list the GLSAs first, each affected package with its fixes") {
    std::ostringstream out;
    egraph::human_notices(
        out,
        std::vector<std::string>{"2026-09-01-x\tnews\tgentoo\tX happened",
                                 "202601-01\tglsa\tfoo: overflow\tdev-libs/foo-1\t>=dev-libs/foo-2",
                                 "202601-01\tglsa\tfoo: overflow\tdev-libs/foo-2.5\t"
                                 ">=dev-libs/foo-2.6:2 >=dev-libs/foo-3",
                                 "202602-01\tglsa\tbar: leak\tdev-libs/bar-1\t"},
        plain, egraph::Seconds{});
    CHECK(out.str() == "Security advisories (egraph install -1 the fixed versions):\n"
                       "  202601-01  foo: overflow\n"
                       "    dev-libs/foo-1, fixed in >=dev-libs/foo-2\n"
                       "    dev-libs/foo-2.5, fixed in >=dev-libs/foo-2.6:2 or >=dev-libs/foo-3\n"
                       "  202602-01  bar: leak\n"
                       "    dev-libs/bar-1\n"
                       "\nUnread news (eselect news read):\n  2026-09-01-x  X happened\n");
}

TEST_CASE("notices list stale repositories, masked packages and missing libraries last") {
    using namespace std::chrono;
    std::ostringstream out;
    egraph::human_notices(out,
                          std::vector<std::string>{"2026-09-01-x\tnews\tgentoo\tX happened",
                                                   "gentoo\tstale\t1790000000",
                                                   "app-misc/b-1\tmasked\tpackage.mask",
                                                   "app-misc/a-1\tmissing\tx86_64\tlibgone.so.2"},
                          plain, egraph::Seconds{seconds{1790000000}} + days{9});
    CHECK(out.str() == "Unread news (eselect news read):\n  2026-09-01-x  X happened\n"
                       "\nStale repositories (egraph sync):\n  gentoo  synced 9 days ago\n"
                       "\nMasked installed packages:\n  app-misc/b-1  package.mask\n"
                       "\nMissing libraries (rebuild what needs them):\n"
                       "  app-misc/a-1  needs libgone.so.2 (x86_64)\n");
}

TEST_CASE("a verified removal says emerge removes the same") {
    std::ostringstream same;
    egraph::human_verification(same, {}, plain, "removes");
    CHECK(same.str() == "\n+ emerge --pretend removes the same.\n");
    std::ostringstream differ;
    egraph::human_verification(differ, std::vector<std::string>{"a/b-1\temerge\tremove"}, plain,
                               "removes");
    CHECK(differ.str() ==
          "\n! emerge --pretend removes otherwise:\n  a/b-1  only in emerge (remove)\n");
}

TEST_CASE("a verified plan's blocker and refusal differences have no repository") {
    std::ostringstream differ;
    egraph::human_verification(differ,
                               std::vector<std::string>{
                                   "a/holder-1\tegraph\tblocks a/x",
                                   "dev-libs/missing\temerge\tunsatisfied",
                               },
                               plain);
    CHECK(differ.str() == "\n! emerge --pretend merges otherwise:\n"
                          "  a/holder-1        only here (blocks a/x)\n"
                          "  dev-libs/missing  only in emerge (unsatisfied)\n");
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

TEST_CASE("updates with lines tried mark what they add and change, then list what they drop") {
    std::ostringstream out;
    egraph::human_updates(
        out,
        std::vector<std::string>{
            "app-misc/up-1\tupgrade\tapp-misc/up-10\ttest_repo",
            "sys-devel/gcc-16\trebuild\tsys-devel/gcc-16\ttest_repo\t-nls*",
            "app-misc/use-1\trebuild\tapp-misc/use-1\ttest_repo\ta*",
            "app-misc/gone-1\ttried\tdropped\tupgrade\tapp-misc/gone-2\ttest_repo\t",
            "sys-devel/gcc-16\ttried\tadded\trebuild\tsys-devel/gcc-16\ttest_repo\t-nls*",
            "app-misc/use-1\ttried\tchanged\trebuild\tapp-misc/use-1\ttest_repo\ta*",
        },
        plain);
    CHECK(out.str() == "  U app-misc/up    1  > 10  ::test_repo\n"
                       "+ R sys-devel/gcc  16       ::test_repo  -nls*\n"
                       "~ R app-misc/use   1        ::test_repo  a*\n"
                       "- U app-misc/gone  1  > 2   ::test_repo  (dropped)\n"
                       "\n1 upgrade, 2 rebuilds\n"
                       "Tried: 1 more merge, 1 fewer, 1 changed, 2 rebuilt for USE\n"
                       "\nflag* changed  flag% new in IUSE  (-flag%) gone from it\n");
    std::ostringstream same;
    egraph::human_updates(same, std::vector<std::string>{"\ttried\tnone"}, plain);
    CHECK(same.str() == "+ Nothing to update.\n\nTried: the plan is the same\n");
}

TEST_CASE("updates list what an env file tried builds otherwise, and how to rebuild it") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{"\ttried\tnone",
                                                   "app-editors/ned-1\tenv\t\tclang.conf",
                                                   "app-misc/b-2\tenv\tkeep.conf\t"},
                          plain);
    CHECK(out.str() == "+ Nothing to update.\n"
                       "\nBuilt differently from now on (package.env)\n"
                       "  app-editors/ned-1  clang.conf (was none)\n"
                       "  app-misc/b-2       no env file (was keep.conf)\n"
                       "rebuild them: --rebuild-env\n"
                       "\nTried: the plan is the same\n");
    std::ostringstream rebuilt;
    egraph::human_updates(
        rebuilt,
        std::vector<std::string>{"app-editors/ned-1\trebuild\tapp-editors/ned-1\tr",
                                 "app-editors/ned-1\ttried\tadded\trebuild\tapp-editors/ned-1\tr\t",
                                 "app-editors/ned-1\tenv\t\tclang.conf"},
        plain);
    CHECK(rebuilt.str().find("--rebuild-env") == std::string::npos);
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

TEST_CASE("held updates left out are counted, for --held to list") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{"a/up-1\tupgrade\ta/up-2\tr",
                                                   "a/b-1\theld\ta/b-2\tr\t\ta/b-2 a/missing"},
                          plain, false, 2);
    CHECK(out.str() == "U a/up  1 > 2  ::r\n"
                       "\nHeld back\n"
                       "H a/b   1 > 2  ::r\n"
                       "    a/b-2  a/missing\n"
                       "\n1 upgrade, 3 held (--held lists the other 2)\n");
    std::ostringstream none_listed;
    egraph::human_updates(none_listed, std::vector<std::string>{}, plain, false, 1);
    CHECK(none_listed.str() == "+ Nothing to update.\n"
                               "\n1 held (--held lists it)\n");
    std::ostringstream some;
    egraph::human_updates(some, std::vector<std::string>{"a/up-1\tupgrade\ta/up-2\tr"}, plain,
                          false, 2);
    CHECK(some.str() == "U a/up  1 > 2  ::r\n"
                        "\n1 upgrade, 2 held (--held lists them)\n");
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

TEST_CASE("a package new in its slot names the installed versions in its other slots") {
    std::ostringstream out;
    egraph::human_updates(
        out,
        std::vector<std::string>{
            "dev-cpp/glibmm-2.66\tupgrade\tdev-cpp/glibmm-2.66-r1\tgentoo",
            "media-video/ff-151\tnew-slot\tmedia-video/ff-151\tgentoo\t\twww-client/opera-136 "
            "media-video/ff:151\tmedia-video/ff-150.0.7871.124:150 media-video/ff-152:152",
            "x/fresh-1\tnew\tx/fresh-1\tgentoo",
            "x/lone-2\tnew-slot\tx/lone-2\tgentoo\t\t\tx/lone-1:1",
        },
        plain);
    CHECK(out.str() == "U dev-cpp/glibmm  2.66 > 2.66-r1  ::gentoo\n"
                       "\nNew\n"
                       "N media-video/ff  151             ::gentoo  www-client/opera-136 "
                       "media-video/ff:151  beside 150.0.7871.124:150 152:152\n"
                       "N x/fresh         1               ::gentoo\n"
                       "N x/lone          2               ::gentoo  beside 1:1\n"
                       "\n1 upgrade, 1 new, 2 in new slots\n");
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

TEST_CASE("the table numbers merges in order, with the earlier merges each waits for") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "1\t3r\tdev-libs/chain-1\tnew\tdev-libs/chain-1\tgentoo\t\tdev-cpp/"
                              "mm-common-1 dev-libs/chain",
                              "2\t1p 3r\tdev-cpp/mm-common-1\tnew\tdev-cpp/mm-common-1\tgentoo\t"
                              "USE=\"doc\"\ta/glibmm-2 dev-cpp/mm-common",
                              "3\t1l 2b\ta/glibmm-1\tupgrade\ta/glibmm-2\tgentoo",
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
        "3\t2r\tx/mm-1\tnew\tx/mm-1\tgentoo\tUSE=\"doc\"\ta/glibmm-2 x/mm",
        "4\t1p 3b\ta/glibmm-1\tupgrade\ta/glibmm-2\tgentoo",
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
                       "   `- U a/glibmm  1 > 2  ::gentoo  4  w 1 3\n"
                       "      `- N x/mm  1  ::gentoo  3  w 2\n"
                       "         `- N x/chain  1  ::gentoo  2\n"
                       "\n2 upgrades, 2 new\n");
}

TEST_CASE("the update tree names a new slot's installed neighbours") {
    const std::vector<std::string> table{
        "1\t\tx/py-3\tnew-slot\tx/py-3\tgentoo\t\ta/top-2 x/py:3\tx/py-2:2",
        "2\t1r\ta/top-1\tupgrade\ta/top-2\tgentoo",
    };
    const std::vector<std::string> tree{
        "1\t@selected\ta/top-1\tx/py-3",
        "2\t@selected\ta/top-1",
    };
    std::ostringstream out;
    egraph::human_update_tree(out, table, tree, plain);
    CHECK(out.str() == "@ @selected\n"
                       "`- U a/top  1 > 2  ::gentoo  2  w 1\n"
                       "   `- N x/py  3  ::gentoo  1  beside 2:2\n"
                       "\n1 upgrade, 1 in a new slot\n");
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

TEST_CASE("a replaced slot is uninstalled, replaced by the merge of its new slot") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "1\t\ta/wine-9\tnew-slot\ta/wine-9\tgentoo\t\t\ta/wine-8:8",
                              "\t1\ta/wine-8\tuninstall\t\t\ta/wine-9",
                          },
                          plain, true);
    CHECK(out.str().contains("Uninstalled\n- a/wine-8  replaced by a/wine-9  w 1\n"));
    CHECK(out.str().ends_with("1 uninstall\n"));
}

TEST_CASE("masked installed packages are warned of, each package.mask comment once") {
    std::ostringstream out;
    egraph::human_updates(
        out,
        std::vector<std::string>{
            "a/user-1\tupgrade\ta/user-2\tgentoo",
            "dev-tex/biber-2.21\tmasked\tgentoo\tpackage.mask\t/repo/profiles/package.mask"
            "\t# A Developer (2026-10-02)\t# TeX Live 2026 is masked for testing.",
            "dev-tex/biblatex-3.21\tmasked\tgentoo\tpackage.mask\t/repo/profiles/package.mask"
            "\t# A Developer (2026-10-02)\t# TeX Live 2026 is masked for testing.",
            "a/eula-1\tmasked\tgentoo\tEULA license(s)\t",
        },
        plain);
    CHECK(out.str() == "U a/user  1 > 2  ::gentoo\n"
                       "\n"
                       "Masked, installed\n"
                       "! dev-tex/biber-2.21::gentoo  masked by package.mask\n"
                       "    /repo/profiles/package.mask:\n"
                       "    # A Developer (2026-10-02)\n"
                       "    # TeX Live 2026 is masked for testing.\n"
                       "! dev-tex/biblatex-3.21::gentoo  masked by package.mask\n"
                       "! a/eula-1::gentoo  masked by EULA license(s)\n"
                       "\n"
                       "1 upgrade, 3 masked\n");
}

TEST_CASE("masked installed packages are warned of with nothing to update") {
    std::ostringstream out;
    egraph::human_updates(
        out, std::vector<std::string>{"a/eula-1\tmasked\tgentoo\tEULA license(s)\t"}, plain);
    CHECK(out.str() == "+ Nothing to update.\n"
                       "\n"
                       "Masked, installed\n"
                       "! a/eula-1::gentoo  masked by EULA license(s)\n"
                       "\n"
                       "1 masked\n");
}

TEST_CASE("the table lists the merges each uninstall waits for") {
    std::ostringstream out;
    egraph::human_updates(out,
                          std::vector<std::string>{
                              "1\t\ta/new-1\tnew\ta/new-1\tgentoo",
                              "2\t1r\ta/user-1\tupgrade\ta/user-2\tgentoo",
                              "\t1 2\ta/old-1\tuninstall\ta/new-1\t!a/old\ta/old-1",
                          },
                          plain, true);
    CHECK(out.str() == "1 N a/new   1      ::gentoo\n"
                       "2 U a/user  1 > 2  ::gentoo  w 1\n"
                       "\n"
                       "Uninstalled\n"
                       "- a/old-1  blocked by a/new-1 !a/old  w 1 2\n"
                       "\n"
                       "1 upgrade, 1 new, 1 uninstall\n");
}

TEST_CASE("updates end with the dependencies nothing satisfies") {
    const std::vector<std::string> records{
        "a/upd-1\tupgrade\ta/upd-2\tgentoo",
        "dev-libs/end-1\tunsatisfied\tdev-libs/missing",
        "a/wants-1\tunsatisfied\t>=dev-libs/testing-2",
    };
    std::ostringstream out;
    egraph::human_updates(out, records, plain);
    CHECK(out.str() == "U a/upd  1 > 2  ::gentoo\n"
                       "\n"
                       "Unsatisfied\n"
                       "! dev-libs/end-1  needs dev-libs/missing\n"
                       "! a/wants-1       needs >=dev-libs/testing-2\n"
                       "no visible version matches: emerge refuses the plan\n"
                       "\n"
                       "1 upgrade, 2 unsatisfied\n");
    // With nothing to merge, not "Nothing to update".
    std::ostringstream alone;
    egraph::human_updates(alone, std::span(records).subspan(1), plain);
    CHECK(alone.str() == "\n"
                         "Unsatisfied\n"
                         "! dev-libs/end-1  needs dev-libs/missing\n"
                         "! a/wants-1       needs >=dev-libs/testing-2\n"
                         "no visible version matches: emerge refuses the plan\n"
                         "\n"
                         "2 unsatisfied\n");
}

TEST_CASE("updates end with the REQUIRED_USE their USE leaves unmet") {
    const std::vector<std::string> records{
        "a/req-1\tnew\ta/req-1\tgentoo\tUSE=\"-a -b\"\t",
        "a/req-1\trequired-use\tgentoo\tUSE=\"-a -b\"\t^^ ( a b )\t",
        "a/cond-1\trequired-use\tgentoo\t\tx? ( || ( a b ) )\tx? ( || ( a b ) ) !x? ( b )",
    };
    std::ostringstream out;
    egraph::human_updates(out, std::span(records).subspan(1), plain);
    CHECK(out.str() == "\n"
                       "Unmet REQUIRED_USE\n"
                       "! a/req-1::gentoo  USE=\"-a -b\"\n"
                       "    exactly-one-of ( a b )\n"
                       "! a/cond-1::gentoo\n"
                       "    x? ( any-of ( a b ) )\n"
                       "    of x? ( any-of ( a b ) ) !x? ( b )\n"
                       "its USE leaves REQUIRED_USE unsatisfied: emerge refuses the plan\n"
                       "\n"
                       "2 unmet\n");
}

TEST_CASE("updates end with the USE changes emerge asks for, as it words them") {
    const std::vector<std::string> records{
        "dev-libs/lib-2\tuse-change\tgentoo\t>=dev-libs/lib-2 gtk"
        "\ta/want-1::gentoo\ta/want (argument)",
    };
    std::ostringstream out;
    egraph::human_updates(out, records, plain);
    CHECK(out.str() == "\n"
                       "USE changes needed\n"
                       "# required by a/want-1::gentoo\n"
                       "# required by a/want (argument)\n"
                       ">=dev-libs/lib-2 gtk\n"
                       "emerge refuses the plan until package.use makes them\n"
                       "\n"
                       "1 USE change\n");
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

TEST_CASE("use lists each ebuild's flags, USE_EXPAND ones grouped, and where each was set") {
    const std::vector<std::string> groups{"VIDEO_CARDS"};
    const std::vector<std::string> records{
        "x/a-1::r\tidefault\tiuse\tpkginternal\t\tidefault",
        "x/a-1::r\t-conf\tiuse\tpkg\t/etc/portage/package.use:4\t-conf",
        "x/a-1::r\t(-m)\tiuse\tmask\t/var/db/repos/gentoo/profiles/base/use.mask:9\tm",
        "x/a-1::r\t-u\tiuse\tmask\t/etc/portage/profile/use.mask:1\t-u",
        "x/a-1::r\t-never\tiuse\t\t\t",
        "x/a-1::r\tx86\timplicit\tarch\t\tx86",
        "x/a-1::r\tvideo_cards_nvidia\tiuse\tpkg\t/etc/portage/package.use:4\tvideo_cards_nvidia",
        "x/a-1::r\t-video_cards_vesa\tiuse\tconf\t/etc/portage/make.conf:15\tVIDEO_CARDS",
        "x/b-1::r\ttest\tiuse\tfeatures\t\ttest",
    };
    const std::vector<egraph::UseVersion> versions{
        {.key = "x/a-1::r", .cp = "x/a", .slot = "0", .installed = true, .pick = true},
        {.key = "x/b-1::r", .cp = "x/b", .slot = "0", .installed = false, .pick = true},
    };
    std::ostringstream out;
    egraph::human_use(out, records, versions, groups, plain);
    CHECK(out.str() == "x/a-1::r\n"
                       "  idefault  IUSE default\n"
                       "  -conf     /etc/portage/package.use:4\n"
                       "  (-m)      masked  gentoo/profiles/base/use.mask:9\n"
                       "  -u        unmasked  /etc/portage/profile/use.mask:1\n"
                       "  -never    not set\n"
                       "  VIDEO_CARDS\n"
                       "    nvidia  /etc/portage/package.use:4\n"
                       "    -vesa   /etc/portage/make.conf:15  (VIDEO_CARDS=)\n"
                       "\n"
                       "x/b-1::r\n"
                       "  test  FEATURES=test\n");
}

TEST_CASE("use shares a package's table among its versions, footnoting what differs") {
    const std::vector<std::string> groups{"VIDEO_CARDS"};
    std::vector<std::string> records;
    const auto add = [&](std::string_view version, std::string_view row) {
        records.push_back(std::format("x/p-{}::r\t{}", version, row));
    };
    for (const auto version : {"1.1", "1.2", "2.1", "2.2"}) {
        add(version, "a\tiuse\tconf\t/etc/portage/make.conf:9\ta");
        add(version, version < std::string_view{"2"}
                         ? "b\tiuse\tconf\t/etc/portage/make.conf:9\tb"
                         : "b\tiuse\tpkg\t/etc/portage/package.use:3\tb");
    }
    add("2.1", "-new\tiuse\t\t\t");
    add("2.2", "-new\tiuse\t\t\t");
    for (const auto version : {"1.1", "1.2", "2.1"}) {
        add(version, "-old\tiuse\t\t\t");
    }
    add("2.2", "video_cards_nvidia\tiuse\tpkg\t/etc/portage/package.use:4\tvideo_cards_nvidia");
    records.emplace_back("x/q-1::r\tc\tiuse\tconf\t/etc/portage/make.conf:9\tc");
    records.emplace_back("x/q-1::s\tc\tiuse\tconf\t/etc/portage/make.conf:9\tc");
    const std::vector<egraph::UseVersion> versions{
        {.key = "x/p-1.1::r", .cp = "x/p", .slot = "1", .installed = true, .pick = false},
        {.key = "x/p-1.2::r", .cp = "x/p", .slot = "1", .installed = false, .pick = false},
        {.key = "x/p-2.1::r", .cp = "x/p", .slot = "2", .installed = true, .pick = false},
        {.key = "x/p-2.2::r", .cp = "x/p", .slot = "2", .installed = false, .pick = true},
        {.key = "x/q-1::r", .cp = "x/q", .slot = "0", .installed = false, .pick = false},
        {.key = "x/q-1::s", .cp = "x/q", .slot = "0", .installed = false, .pick = false},
    };
    std::ostringstream out;
    egraph::human_use(out, records, versions, groups, plain);
    CHECK(out.str() == "x/p::r\n"
                       "  1.1+  1.2  2.1+  2.2*\n"
                       "  + installed  * emerge's pick\n"
                       "\n"
                       "  a             /etc/portage/make.conf:9\n"
                       "  b [1]         /etc/portage/make.conf:9\n"
                       "  b [2]         /etc/portage/package.use:3\n"
                       "  -new [2]      not set\n"
                       "  -old [3]      not set\n"
                       "  VIDEO_CARDS\n"
                       "    nvidia [4]  /etc/portage/package.use:4\n"
                       "\n"
                       "  [1] :1 only   [2] :2 only   [3] not 2.2   [4] 2.2 only\n"
                       "\n"
                       "x/q\n"
                       "  1::r  1::s\n"
                       "\n"
                       "  c  /etc/portage/make.conf:9\n");
}

TEST_CASE("use's footnotes raise their numbers where the glyphs can") {
    const std::vector<std::string> records{
        "x/p-1::r\ta\tiuse\t\t\t",
        "x/p-2::r\ta\tiuse\t\t\t",
        "x/p-2::r\tb\tiuse\t\t\t",
    };
    const std::vector<egraph::UseVersion> versions{
        {.key = "x/p-1::r", .cp = "x/p", .slot = "0", .installed = true, .pick = false},
        {.key = "x/p-2::r", .cp = "x/p", .slot = "0", .installed = false, .pick = true},
    };
    std::ostringstream out;
    egraph::human_use(
        out, records, versions, {},
        {.paint = egraph::Painter{ColorDepth::none}, .glyph_set = egraph::GlyphSet::unicode});
    CHECK(out.str() == "x/p::r\n"
                       "  1●  2★\n"
                       "  ● installed  ★ emerge's pick\n"
                       "\n"
                       "  a    not set\n"
                       "  b ¹  not set\n"
                       "\n"
                       "  ¹ 2 only\n");
}

TEST_CASE("use with a flag lists every step that set it, in the order applied") {
    const std::vector<std::string> records{
        "x/a-2::r\tranged\toff\tpkg\t/etc/portage/package.use:4\t<x/a-3\t-ranged\tunchanged",
        "x/a-2::r\tranged\ton\tpkg\t/etc/portage/package.use:3\t>=x/a-1\tranged\tchanged",
        "x/b-1::r\tranged\toff\t\t\t\t\t",
    };
    std::ostringstream out;
    egraph::human_use_steps(out, records, {}, plain);
    CHECK(out.str() == "x/a-2::r  ranged\n"
                       "  - /etc/portage/package.use:4  <x/a-3  (no change)\n"
                       "  + /etc/portage/package.use:3  >=x/a-1  (more specific)\n"
                       "\n"
                       "x/b-1::r  ranged\n"
                       "  - not set\n");
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
                                glyph.spark,
                                glyph.installed,
                                glyph.pick}) {
            CHECK_FALSE(text.empty());
        }
        // Ten digits, or none for [n].
        const auto digits = std::ranges::count_if(glyph.superscripts, [](char c) {
            return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
        });
        CHECK((digits == 10 || digits == 0));
    }
}

TEST_CASE("versions align under their cp, and say why a masked one is") {
    std::ostringstream out;
    egraph::human_versions(out,
                           std::vector<std::string>{
                               "dev-libs/a-1::gentoo\t0\tvisible",
                               "dev-libs/a-10.2::overlay\t0/2\tmasked\tpackage.mask\t~x86 keyword",
                               "dev-libs/b-1::gentoo\t1\tmasked",
                           },
                           plain);
    CHECK(out.str() == "* dev-libs/a\n"
                       "    1     :0    ::gentoo\n"
                       "    10.2  :0/2  ::overlay  masked: package.mask, ~x86 keyword\n"
                       "* dev-libs/b\n"
                       "    1  :1  ::gentoo  masked\n");
}

TEST_CASE("findings read as a linter's, then how many of each severity") {
    std::ostringstream out;
    egraph::human_findings(
        out,
        std::vector<std::string>{
            "/etc/portage/package.use/x\t3\terror\tdead\tdev-libs/nope\t\tmatches nothing",
            "/etc/portage/package.use/x\t5\twarning\tno-effect\tapp-misc/foo\tbaz\tnot in IUSE",
            "/etc/portage/package.accept_keywords\t0\twarning\tno-effect\tdev-libs/b\t~x86\t"
            "already accepted",
            "/etc/portage/package.use/x\t7\tnote\tnot-installed\tapp-misc/gone\t\tnot installed",
        },
        plain);
    CHECK(out.str() == "/etc/portage/package.use/x:3: error: dev-libs/nope matches nothing\n"
                       "/etc/portage/package.use/x:5: warning: app-misc/foo baz: not in IUSE\n"
                       "/etc/portage/package.accept_keywords: warning: dev-libs/b ~x86: already "
                       "accepted\n"
                       "/etc/portage/package.use/x:7: note: app-misc/gone not installed\n"
                       "\n"
                       "1 error, 2 warnings, 1 note\n");
}

TEST_CASE("no findings says so") {
    std::ostringstream out;
    egraph::human_findings(out, std::vector<std::string>{}, plain);
    CHECK(out.str() == "no findings\n");
}

TEST_CASE("search shows each key's packages with their versions and metadata") {
    std::ostringstream out;
    const std::vector<std::string> keys{"ssl", "none"};
    egraph::human_search(
        out,
        std::vector<std::string>{
            "ssl\tdev-libs/openssl\t3.5\tvisible\t3.4\thttps://o\tApache-2.0\tToolkit",
            "ssl\tdev-libs/new\t2\tmasked\t\t\t\t",
        },
        keys, plain);
    CHECK(out.str() == "? ssl  2 packages\n"
                       "  * dev-libs/openssl\n"
                       "      available  3.5\n"
                       "      installed  3.4\n"
                       "      homepage   https://o\n"
                       "      license    Apache-2.0\n"
                       "      Toolkit\n"
                       "  * dev-libs/new\n"
                       "      available  2 masked\n"
                       "      installed  not installed\n"
                       "\n"
                       "? none  nothing found\n");
}

TEST_CASE("diff lines up versions, the flags after, then each root set's atoms") {
    std::ostringstream out;
    egraph::human_diff(out,
                       std::vector<std::string>{
                           "app-misc/up-1\tupgrade\tapp-misc/up-10\t+x -y",
                           "app-misc/down-2\tdowngrade\tapp-misc/down-1\t",
                           "app-misc/use-1\trebuild\tapp-misc/use-1\t+x",
                           "\tnew\tapp-misc/new-1.2\t",
                           "app-misc/gone-3\tuninstall\t\t",
                           "@selected\tadded\tapp-misc/new",
                           "@selected\tremoved\tapp-misc/gone",
                           "@system\tadded\tapp-misc/up",
                       },
                       "2026-10-05 14:02:11", plain);
    CHECK(out.str() == "Since 2026-10-05 14:02:11\n"
                       "U app-misc/up    1 > 10   +x -y\n"
                       "D app-misc/down  2 > 1\n"
                       "R app-misc/use   1        +x\n"
                       "N app-misc/new     > 1.2\n"
                       "- app-misc/gone  3\n"
                       "@ @selected  +app-misc/new  -app-misc/gone\n"
                       "@ @system    +app-misc/up\n"
                       "\n1 upgrade, 1 downgrade, 1 rebuild, 1 new, 1 uninstalled\n");
}

TEST_CASE("diff says when nothing changed") {
    std::ostringstream out;
    egraph::human_diff(out, {}, "2026-10-05 14:02:11", plain);
    CHECK(out.str() == "+ Nothing changed since 2026-10-05 14:02:11.\n");
    std::ostringstream roots;
    egraph::human_diff(roots, std::vector<std::string>{"@selected\tadded\ta/b"}, "then", plain);
    CHECK(roots.str() == "Since then\n@ @selected  +a/b\n");
}

TEST_CASE("history lines up its events after their times") {
    std::ostringstream out;
    egraph::human_history(out,
                          std::vector<std::string>{
                              "2026-10-01 14:02:11\t\tnew\tapp-misc/foo-1.2\t",
                              "2026-10-02 09:00:00\tapp-misc/foo-1.2\tupgrade\tapp-misc/foo-1.3\t",
                              "2026-10-03 10:00:00\tapp-misc/foo-1.3\trebuild\tapp-misc/foo-1.3\t",
                              "2026-10-04 11:00:00\tdev-libs/gone-2\tuninstall\t\t",
                          },
                          plain);
    CHECK(out.str() == "2026-10-01 14:02:11  N app-misc/foo       > 1.2\n"
                       "2026-10-02 09:00:00  U app-misc/foo   1.2 > 1.3\n"
                       "2026-10-03 10:00:00  R app-misc/foo   1.3\n"
                       "2026-10-04 11:00:00  - dev-libs/gone  2\n");
    std::ostringstream none;
    egraph::human_history(none, {}, plain);
    CHECK(none.str() == "+ Nothing in the history.\n");
}
