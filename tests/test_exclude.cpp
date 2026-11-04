#include "exclude.hpp"
#include "plan.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::test::make_system;

namespace {

std::vector<std::string> texts(const std::vector<egraph::Exclusion>& found) {
    std::vector<std::string> out;
    out.reserve(found.size());
    for (const auto& each : found) {
        out.push_back(each.text);
    }
    return out;
}

} // namespace

TEST_CASE("exclusions take names, slot atoms and wildcards, several to a value") {
    const std::vector<std::string> values{"sys-kernel/gentoo-sources", "vim app-misc/*:2",
                                          "  */*-sources  "};
    const auto found = egraph::parse_exclusions(values);
    REQUIRE(found);
    CHECK(texts(*found) == std::vector<std::string>{"sys-kernel/gentoo-sources", "vim",
                                                    "app-misc/*:2", "*/*-sources"});
}

TEST_CASE("exclusions refuse operators, USE dependencies, repositories and blockers") {
    for (const auto* bad : {">=sys-kernel/gentoo-sources-6", "=app-misc/foo-1", "app-misc/foo[x]",
                            "app-misc/foo::gentoo", "!app-misc/foo", "app-misc/"}) {
        const std::vector<std::string> values{bad};
        INFO(bad);
        const auto found = egraph::parse_exclusions(values);
        REQUIRE_FALSE(found);
        CHECK(found.error().starts_with(bad));
    }
}

TEST_CASE("the exclude list skips comments and names the line it cannot read") {
    const auto found = egraph::parse_exclude_list("# old kernels\n"
                                                  "sys-kernel/gentoo-sources\n"
                                                  "\n"
                                                  "app-misc/foo:1 # one slot\n");
    REQUIRE(found);
    CHECK(texts(*found) == std::vector<std::string>{"sys-kernel/gentoo-sources", "app-misc/foo:1"});
    const auto bad = egraph::parse_exclude_list("app-misc/foo\n>=app-misc/bar-1\n");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().starts_with("line 2: "));
}

TEST_CASE("an exclusion matches installed packages by name, slot and wildcard") {
    const auto system = make_system({{.cpv = "sys-kernel/gentoo-sources-6.1.1", .slot = "6.1.1"},
                                     {.cpv = "sys-kernel/gentoo-sources-6.2.0", .slot = "6.2.0"},
                                     {.cpv = "sys-kernel/vanilla-sources-6.2.0", .slot = "6.2.0"},
                                     {.cpv = "app-misc/foo-1"}},
                                    {});
    const auto matched = [&](const std::string& text) {
        const std::vector<std::string> values{text};
        const auto found = egraph::parse_exclusions(values);
        REQUIRE(found);
        std::vector<std::string> cpvs;
        for (const auto& pkg : system.store.packages) {
            if (egraph::excludes(system.store, pkg, found->front())) {
                cpvs.emplace_back(system.store.string(pkg.cpv));
            }
        }
        return cpvs;
    };
    CHECK(matched("sys-kernel/gentoo-sources") ==
          std::vector<std::string>{"sys-kernel/gentoo-sources-6.1.1",
                                   "sys-kernel/gentoo-sources-6.2.0"});
    CHECK(matched("gentoo-sources:6.1.1") ==
          std::vector<std::string>{"sys-kernel/gentoo-sources-6.1.1"});
    CHECK(matched("sys-kernel/*-sources:6.2.0") ==
          std::vector<std::string>{"sys-kernel/gentoo-sources-6.2.0",
                                   "sys-kernel/vanilla-sources-6.2.0"});
    CHECK(matched("*/foo") == std::vector<std::string>{"app-misc/foo-1"});
    CHECK(matched("app-misc/bar").empty());
}

TEST_CASE("an excluded package is neither updated nor merged, and a slot atom spares the others") {
    const auto system =
        make_system({{.cpv = "app-misc/foo-1", .slot = "1"}, {.cpv = "app-misc/bar-1"}},
                    {{.cpv = "app-misc/foo-1", .slot = "1"},
                     {.cpv = "app-misc/foo-2", .slot = "1"},
                     {.cpv = "app-misc/foo-3", .slot = "3"},
                     {.cpv = "app-misc/bar-1"},
                     {.cpv = "app-misc/bar-2"}});
    const auto trimmed = [&](const std::string& text) {
        const std::vector<std::string> values{text};
        const auto found = egraph::parse_exclusions(values);
        REQUIRE(found);
        return egraph::with_exclusions(system.evaluated, system.store, *found);
    };
    const auto visible = [](const egraph::Evaluated& evaluated) {
        std::vector<std::string> cpvs;
        for (const auto& candidate : evaluated.candidates) {
            if (candidate.visible()) {
                cpvs.emplace_back(evaluated.string(candidate.cpv));
            }
        }
        return cpvs;
    };
    const auto all = trimmed("app-misc/foo");
    CHECK(visible(all) == std::vector<std::string>{"app-misc/bar-1", "app-misc/bar-2"});
    const auto targeted = [&](const egraph::Evaluated& evaluated, const std::string& cpv) {
        for (std::size_t id = 0; id < system.store.packages.size(); ++id) {
            if (system.store.string(system.store.packages.at(id).cpv) == cpv) {
                return evaluated.packages.at(id).target.has_value();
            }
        }
        return false;
    };
    CHECK(targeted(system.evaluated, "app-misc/foo-1"));
    CHECK_FALSE(targeted(all, "app-misc/foo-1"));
    CHECK(targeted(all, "app-misc/bar-1"));
    const auto slot = trimmed("app-misc/foo:3");
    CHECK(visible(slot) == std::vector<std::string>{"app-misc/bar-1", "app-misc/bar-2",
                                                    "app-misc/foo-1", "app-misc/foo-2"});
    CHECK(targeted(slot, "app-misc/foo-1"));
}

TEST_CASE("a plan over excluded candidates merges only what is left") {
    const auto system = make_system({{.cpv = "app-misc/foo-1"}, {.cpv = "app-misc/bar-1"}},
                                    {{.cpv = "app-misc/foo-1"},
                                     {.cpv = "app-misc/foo-2"},
                                     {.cpv = "app-misc/bar-1"},
                                     {.cpv = "app-misc/bar-2"}});
    const std::vector<std::string> values{"app-misc/foo"};
    const auto found = egraph::parse_exclusions(values);
    REQUIRE(found);
    const auto evaluated = egraph::with_exclusions(system.evaluated, system.store, *found);
    const auto plan = egraph::plan_updates(system.store, evaluated, egraph::UseRebuilds::none, {});
    REQUIRE(plan.merges.size() == 1);
    CHECK(evaluated.string(evaluated.candidates.at(plan.merges.front().candidate).cpv) ==
          "app-misc/bar-2");
}
