#include "system_builder.hpp"
#include "use_ledger_builder.hpp"
#include "use_stack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using egraph::test::Available;
using egraph::test::detail::Interner;
using Line = egraph::test::UseLine;
using Files = egraph::test::UseFiles;
using Repository = egraph::test::UseRepository;
using Spec = egraph::test::UseSpec;
using Own = egraph::test::OwnLayers;
using egraph::test::ids_of;
using egraph::test::set_ledger;

namespace {

// Each candidate's stacked USE, from candidates with their IUSE, and their own layers.
struct Stacked {
    egraph::test::System system;
    std::vector<egraph::StackedUse> stacked;

    [[nodiscard]] const egraph::StackedUse& of(std::string_view cpv) const {
        for (std::size_t i = 0; i < system.evaluated.candidates.size(); ++i) {
            if (system.evaluated.string(system.evaluated.candidates.at(i).cpv) == cpv) {
                return stacked.at(i);
            }
        }
        FAIL("no candidate " << cpv);
        return stacked.front();
    }
};

Stacked stack(std::vector<Available> available, const Spec& spec, const std::vector<Own>& own = {},
              const std::vector<std::string>& implicit = {}) {
    Stacked result{.system = egraph::test::make_system({}, std::move(available)), .stacked = {}};
    auto& ev = result.system.evaluated;
    result.system.store.implicit.effective = implicit;
    set_ledger(ev, spec);
    Interner intern(ev);
    for (const auto& layers : own) {
        for (auto& candidate : ev.candidates) {
            if (ev.string(candidate.cpv) == layers.cpv) {
                candidate.stable = layers.stable;
                candidate.internal =
                    ids_of(ev, intern, egraph::test::detail::tokens(layers.internal));
                candidate.features =
                    ids_of(ev, intern, egraph::test::detail::tokens(layers.features));
            }
        }
    }
    const egraph::UseStacker stacker(result.system.store, ev);
    for (const auto& candidate : ev.candidates) {
        result.stacked.push_back(stacker.stack(candidate));
    }
    return result;
}

using Flags = std::vector<std::string>;

} // namespace

TEST_CASE("USE stacks layer by layer, -* clearing what came before") {
    const auto s =
        stack({{.cpv = "cat/a-1", .iuse = "a b c d"}},
              {.profiles = {{{"make.defaults", {{.file = "/p/make.defaults", .tokens = "a b"}}}}},
               .conf = {{.file = "/etc/portage/make.conf", .line = 3, .tokens = "-* c -d"}}});
    const auto& a = s.of("cat/a-1");
    CHECK(a.use == Flags{"c"});
    const auto& steps = a.steps.at("a");
    REQUIRE(steps.size() == 2);
    CHECK(steps.front().layer == egraph::UseLayer::defaults);
    CHECK(steps.front().enabled);
    CHECK(steps.back().layer == egraph::UseLayer::conf);
    CHECK(steps.back().token == "-*");
    CHECK_FALSE(steps.back().enabled);
    // Naming a flag counts even where it changes nothing.
    REQUIRE(a.steps.at("d").size() == 1);
    CHECK_FALSE(a.steps.at("d").front().changed);
    CHECK(a.steps.at("d").front().entry.has_value());
}

TEST_CASE("a token left out stacks as if it were not on its line") {
    const Spec spec{
        .use_expand = {"TARGETS"},
        .unprefixed = {"KERNEL"},
        .profiles = {{{"make.defaults",
                       {{.file = "md", .tokens = "a targets_x"},
                        {.var = "KERNEL", .tokens = "u v"}}}}},
        .package_use = {
            {.file = "pu", .line = 1, .atom = "cat/a", .tokens = "-a b"},
            {.file = "pu", .line = 2, .atom = "cat/a", .tokens = "-targets_* targets_x"}}};
    auto s = stack({{.cpv = "cat/a-1", .iuse = "a b u v targets_x"}}, spec);
    const auto& ev = s.system.evaluated;
    const egraph::UseStacker stacker(s.system.store, ev);
    const auto& candidate = ev.candidates.front();
    const auto use = [&](std::uint32_t entry, std::uint32_t position) {
        return stacker.stack(candidate, egraph::UseStacker::Omitted{entry, position}).use;
    };
    const auto pu = ev.ledger.package_use.first;
    CHECK(stacker.stack(candidate).use == Flags{"b", "targets_x", "u", "v"});
    CHECK(use(pu, 0) == Flags{"a", "b", "targets_x", "u", "v"});
    CHECK(use(pu, 1) == Flags{"targets_x", "u", "v"});
    // After -*, x alone sets it.
    CHECK(use(pu + 1, 1) == Flags{"b", "u", "v"});
    const auto profile = ev.ledger.profiles.front().sources.at(0);
    CHECK(use(profile.first + 1, 0) == Flags{"b", "targets_x", "v"});
}

TEST_CASE("package.use outranks IUSE defaults, and the most specific atom wins") {
    const Spec spec{.package_use = {{.file = "pu", .line = 1, .atom = ">=cat/a-1", .tokens = "x"},
                                    {.file = "pu", .line = 2, .atom = "<cat/a-3", .tokens = "-x"},
                                    {.file = "pu", .line = 3, .atom = "cat/a", .tokens = "-d"}}};
    const auto s = stack({{.cpv = "cat/a-0.5", .iuse = "x d"},
                          {.cpv = "cat/a-2", .iuse = "x d"},
                          {.cpv = "cat/a-3", .iuse = "x d"}},
                         spec,
                         {{.cpv = "cat/a-0.5", .internal = "d"},
                          {.cpv = "cat/a-2", .internal = "d"},
                          {.cpv = "cat/a-3", .internal = "d"}});
    CHECK(s.of("cat/a-0.5").use.empty());
    // >=cat/a-1 and <cat/a-3 tie; the version next to 2 is 1, so >= wins.
    CHECK(s.of("cat/a-2").use == Flags{"x"});
    CHECK(s.of("cat/a-3").use == Flags{"x"});
    CHECK(s.of("cat/a-2").steps.at("d").front().layer == egraph::UseLayer::pkginternal);
}

TEST_CASE("a USE_EXPAND variable replaces the flags of its prefix") {
    const Spec spec{
        .use_expand = {"VIDEO_CARDS"},
        .profiles = {{{"make.defaults",
                       {{.file = "md", .var = "VIDEO_CARDS", .tokens = "video_cards_vesa"}}}}},
        .conf = {{.file = "mc", .line = 2, .var = "VIDEO_CARDS", .tokens = "radeon"}},
        .package_use = {
            {.file = "pu", .atom = "cat/a", .tokens = "-video_cards_* video_cards_nvidia"}}};
    const auto s =
        stack({{.cpv = "cat/a-1", .iuse = "video_cards_vesa video_cards_radeon video_cards_nvidia"},
               {.cpv = "cat/b-1", .iuse = "video_cards_vesa video_cards_radeon"}},
              spec);
    CHECK(s.of("cat/a-1").use == Flags{"video_cards_nvidia"});
    CHECK(s.of("cat/b-1").use == Flags{"video_cards_radeon"});
    const auto& vesa = s.of("cat/b-1").steps.at("video_cards_vesa");
    REQUIRE(vesa.size() == 2);
    CHECK(vesa.back().token == "VIDEO_CARDS");
    CHECK(vesa.back().layer == egraph::UseLayer::conf);
}

TEST_CASE("masks beat forces, and a later profile node takes either back") {
    const Spec spec{.profiles = {{{"use.force", {{.file = "f", .tokens = "f m"}}},
                                  {"use.mask", {{.file = "m", .tokens = "m g"}}}},
                                 {{"use.mask", {{.file = "m2", .tokens = "-g"}}}}},
                    .conf = {{.file = "mc", .tokens = "g"}}};
    const auto s = stack({{.cpv = "cat/a-1", .iuse = "f m g"}}, spec);
    const auto& a = s.of("cat/a-1");
    CHECK(a.use == Flags{"f", "g"});
    CHECK(a.forced == Flags{"f", "m"});
    CHECK(a.steps.at("m").back().layer == egraph::UseLayer::mask);
    // g's mask, taken back, is its last step: it changed nothing.
    const auto& g = a.steps.at("g").back();
    CHECK(g.layer == egraph::UseLayer::mask);
    CHECK(g.token == "-g");
    CHECK(g.enabled);
    CHECK_FALSE(g.changed);
}

TEST_CASE("the stable files apply to stable ebuilds alone") {
    const Spec spec{
        .profiles = {{{"use.stable", {{.file = "us", .tokens = "s"}}},
                      {"package.use.stable", {{.file = "pus", .atom = "cat/a", .tokens = "t"}}},
                      {"use.stable.mask", {{.file = "usm", .tokens = "u"}}}}},
        .conf = {{.file = "mc", .tokens = "u"}}};
    const auto s = stack({{.cpv = "cat/a-1", .iuse = "s t u"}, {.cpv = "cat/a-2", .iuse = "s t u"}},
                         spec, {{.cpv = "cat/a-1", .stable = true}});
    CHECK(s.of("cat/a-1").use == Flags{"s", "t"});
    CHECK(s.of("cat/a-1").forced == Flags{"u"});
    CHECK(s.of("cat/a-2").use == Flags{"u"});
}

TEST_CASE("a repository's files stack after its masters'") {
    const Spec spec{
        .repositories = {{.name = "gentoo",
                          .masters = {},
                          .files = {{"make.defaults", {{.file = "g/md", .tokens = "r"}}},
                                    {"use.mask", {{.file = "g/um", .tokens = "o"}}}}},
                         {.name = "overlay",
                          .masters = {"gentoo"},
                          .files = {{"use.mask", {{.file = "o/um", .tokens = "-o"}}}}}},
        .conf = {{.file = "mc", .tokens = "o"}}};
    const auto s = stack({{.cpv = "cat/a-1", .repo = "overlay", .iuse = "o r"},
                          {.cpv = "cat/b-1", .repo = "gentoo", .iuse = "o r"}},
                         spec);
    CHECK(s.of("cat/a-1").use == Flags{"o", "r"});
    CHECK(s.of("cat/b-1").use == Flags{"r"});
    CHECK(s.of("cat/b-1").forced == Flags{"o"});
}

TEST_CASE("a prefix wildcard expands against IUSE") {
    const Spec spec{.conf = {{.file = "mc", .tokens = "linguas_* -linguas_en"}}};
    const auto s = stack(
        {{.cpv = "cat/a-1", .iuse = "linguas_en linguas_de"}, {.cpv = "cat/b-1", .iuse = "x"}},
        spec);
    CHECK(s.of("cat/a-1").use == Flags{"linguas_de"});
    CHECK(s.of("cat/b-1").use.empty());
}

TEST_CASE("package.env's files come before package.use in the package layer") {
    const Spec spec{.package_use = {{.file = "pu", .atom = "cat/b", .tokens = "-fromenv"}},
                    .package_env = {{.file = "pe", .atom = "cat/*", .tokens = "e.conf"}},
                    .env_files = {{"e.conf", {{.file = "env/e.conf", .tokens = "fromenv"}}}}};
    const auto s =
        stack({{.cpv = "cat/a-1", .iuse = "fromenv"}, {.cpv = "cat/b-1", .iuse = "fromenv"}}, spec);
    CHECK(s.of("cat/a-1").use == Flags{"fromenv"});
    CHECK(s.of("cat/b-1").use.empty());
}

TEST_CASE("ARCH and implicit flags pass the IUSE filter, others do not") {
    const Spec spec{.arch = "x86", .conf = {{.file = "mc", .tokens = "stray"}}};
    const auto s = stack({{.cpv = "cat/a-1", .iuse = "x"}}, spec, {}, {"x86"});
    CHECK(s.of("cat/a-1").use == Flags{"x86"});
    CHECK(s.of("cat/a-1").steps.at("x86").back().layer == egraph::UseLayer::arch);
}
