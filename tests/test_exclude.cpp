#include "exclude.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::test::make_system;

namespace {

std::vector<std::string> texts(const std::vector<egraph::Exclusion>& found) {
    std::vector<std::string> out;
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
