#include "system_builder.hpp"
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

namespace {

// One ledger entry: tokens are space-separated.
struct Line {
    std::string file = {};
    std::uint32_t line = 1;
    std::string atom = {};
    std::string var = "USE";
    std::string tokens = {};
};

// Entries by the name of one of ledger_files.
using Files = std::map<std::string, std::vector<Line>, std::less<>>;

struct Repository {
    std::string name = {};
    std::vector<std::string> masters = {};
    Files files = {};
};

struct Spec {
    std::string use_order = "env pkg conf defaults pkginternal features repo env.d";
    std::vector<std::string> use_expand = {};
    std::vector<std::string> unprefixed = {};
    std::string arch = {};
    std::vector<Files> profiles = {};
    std::vector<Repository> repositories = {};
    std::vector<Line> conf = {};
    std::vector<Line> package_use = {};
    std::vector<Line> package_env = {};
    std::vector<std::pair<std::string, std::vector<Line>>> env_files = {};
};

// The per-package layers of a candidate.
struct Own {
    std::string cpv = {};
    bool stable = false;
    std::string internal = {};
    std::string features = {};
};

egraph::Range ids_of(egraph::Evaluated& ev, Interner& intern,
                     const std::vector<std::string>& words) {
    const egraph::Range range{.first = static_cast<std::uint32_t>(ev.ids.size()),
                              .count = static_cast<std::uint32_t>(words.size())};
    for (const auto& word : words) {
        ev.ids.push_back(intern(word));
    }
    return range;
}

egraph::Range entries_of(egraph::Evaluated& ev, Interner& intern, const std::vector<Line>& lines) {
    // Tokens first: ids and entries are separate tables, but each entry's range must be final.
    std::vector<egraph::LedgerEntry> made;
    for (const auto& line : lines) {
        made.push_back({.file = intern(line.file),
                        .line = line.line,
                        .atom = intern(line.atom),
                        .var = intern(line.var),
                        .tokens = ids_of(ev, intern, egraph::test::detail::tokens(line.tokens))});
    }
    const egraph::Range range{.first = static_cast<std::uint32_t>(ev.ledger_entries.size()),
                              .count = static_cast<std::uint32_t>(made.size())};
    ev.ledger_entries.insert(ev.ledger_entries.end(), made.begin(), made.end());
    return range;
}

egraph::LedgerSources sources_of(egraph::Evaluated& ev, Interner& intern, const Files& files) {
    egraph::LedgerSources sources{};
    for (std::size_t i = 0; i < egraph::ledger_files.size(); ++i) {
        const auto found = files.find(egraph::ledger_files.at(i));
        sources.at(i) =
            entries_of(ev, intern, found == files.end() ? std::vector<Line>{} : found->second);
    }
    return sources;
}

void set_ledger(egraph::Evaluated& ev, const Spec& spec) {
    Interner intern(ev);
    auto& ledger = ev.ledger;
    ledger.use_order = ids_of(ev, intern, egraph::test::detail::tokens(spec.use_order));
    ledger.use_expand = ids_of(ev, intern, spec.use_expand);
    ledger.use_expand_unprefixed = ids_of(ev, intern, spec.unprefixed);
    ledger.arch = intern(spec.arch);
    for (const auto& files : spec.profiles) {
        ledger.profiles.push_back(
            {.path = intern("/profile"), .sources = sources_of(ev, intern, files)});
    }
    for (const auto& repo : spec.repositories) {
        ledger.repositories.push_back({.name = intern(repo.name),
                                       .masters = ids_of(ev, intern, repo.masters),
                                       .sources = sources_of(ev, intern, repo.files)});
    }
    ledger.conf = entries_of(ev, intern, spec.conf);
    ledger.package_use = entries_of(ev, intern, spec.package_use);
    ledger.package_env = entries_of(ev, intern, spec.package_env);
    for (const auto& [name, lines] : spec.env_files) {
        ledger.env_files.push_back(
            {.name = intern(name), .entries = entries_of(ev, intern, lines)});
    }
}

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
