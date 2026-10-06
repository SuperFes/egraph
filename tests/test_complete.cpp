#include "complete.hpp"

#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using egraph::Completing;
using egraph::test::make_system;
using Words = std::vector<std::string>;

namespace {

egraph::test::System sample() {
    auto system = make_system({{.cpv = "dev-libs/openssl-3.4.0", .slot = "0"},
                               {.cpv = "dev-libs/openssl-1.1.1w", .slot = "1.1"},
                               {.cpv = "sys-kernel/gentoo-sources-6.12.1", .slot = "6.12.1"}},
                              {{.cpv = "dev-libs/openssl-3.5.0", .slot = "0"},
                               {.cpv = "dev-libs/openssl-3.4.0", .slot = "0"},
                               {.cpv = "dev-libs/openssl-compat-1.0", .repo = "overlay"},
                               {.cpv = "sys-kernel/gentoo-sources-6.12.1", .slot = "6.12.1"}});
    egraph::test::add_repository_cps(system, {"dev-libs/openssl", "dev-libs/openssl-compat",
                                              "dev-libs/libfoo", "sys-kernel/gentoo-sources",
                                              "net-misc/openssh", "virtual/openssh"});
    return system;
}

Words complete(Completing what, std::string_view word) {
    static const auto system = sample();
    return egraph::complete_word(system.store, system.evaluated, what, word);
}

} // namespace

TEST_CASE("an empty word completes to the categories") {
    CHECK(complete(Completing::installed, "") == Words{"dev-libs/", "sys-kernel/"});
    CHECK(complete(Completing::atoms, "") ==
          Words{"dev-libs/", "net-misc/", "sys-kernel/", "virtual/"});
}

TEST_CASE("a word without a category completes to categories, and for atoms to names") {
    CHECK(complete(Completing::installed, "s") == Words{"sys-kernel/"});
    CHECK(complete(Completing::atoms, "open") == Words{"openssh", "openssl", "openssl-compat"});
    CHECK(complete(Completing::atoms, "d") == Words{"dev-libs/"});
    CHECK(complete(Completing::installed, "open").empty());
}

TEST_CASE("a word with a category completes to the cps, the installed or the repositories'") {
    CHECK(complete(Completing::installed, "dev-libs/") == Words{"dev-libs/openssl"});
    CHECK(complete(Completing::atoms, "dev-libs/") ==
          Words{"dev-libs/libfoo", "dev-libs/openssl", "dev-libs/openssl-compat"});
    CHECK(complete(Completing::atoms, "dev-libs/openssl-") == Words{"dev-libs/openssl-compat"});
}

TEST_CASE("an installed word past its cp completes to the installed cpvs") {
    CHECK(complete(Completing::installed, "dev-libs/openssl") == Words{"dev-libs/openssl"});
    CHECK(complete(Completing::installed, "dev-libs/openssl-") ==
          Words{"dev-libs/openssl-1.1.1w", "dev-libs/openssl-3.4.0"});
    CHECK(complete(Completing::installed, "dev-libs/openssl-3") == Words{"dev-libs/openssl-3.4.0"});
}

TEST_CASE("after an operator, the versions") {
    CHECK(complete(Completing::installed, "=dev-libs/openssl-") ==
          Words{"=dev-libs/openssl-1.1.1w", "=dev-libs/openssl-3.4.0"});
    CHECK(complete(Completing::atoms, ">=dev-libs/openssl-3") ==
          Words{">=dev-libs/openssl-3.4.0", ">=dev-libs/openssl-3.5.0"});
    CHECK(complete(Completing::atoms, "=") ==
          Words{"=dev-libs/", "=net-misc/", "=sys-kernel/", "=virtual/"});
    // Without a category, as requests fill it in.
    CHECK(complete(Completing::atoms, "~openssl-3.5") == Words{"~openssl-3.5.0"});
    CHECK(complete(Completing::installed, "=openssl").empty());
}

TEST_CASE("after a colon, the slots") {
    CHECK(complete(Completing::installed, "dev-libs/openssl:") ==
          Words{"dev-libs/openssl:0", "dev-libs/openssl:1.1"});
    CHECK(complete(Completing::installed, "dev-libs/openssl:1") == Words{"dev-libs/openssl:1.1"});
    CHECK(complete(Completing::atoms, "gentoo-sources:") == Words{"gentoo-sources:6.12.1"});
    CHECK(complete(Completing::atoms, "=dev-libs/openssl-3.5.0:") ==
          Words{"=dev-libs/openssl-3.5.0:0"});
}

TEST_CASE("after two colons, the repositories") {
    CHECK(complete(Completing::atoms, "dev-libs/openssl::") ==
          Words{"dev-libs/openssl::overlay", "dev-libs/openssl::test_repo"});
    CHECK(complete(Completing::atoms, "dev-libs/openssl:0::o") ==
          Words{"dev-libs/openssl:0::overlay"});
}

TEST_CASE("after @, the sets, for atoms") {
    CHECK(complete(Completing::atoms, "@s") == Words{"@selected", "@system"});
    CHECK(complete(Completing::atoms, "@").size() == 5);
    CHECK(complete(Completing::installed, "@").empty());
}

TEST_CASE("repositories by name") {
    CHECK(complete(Completing::repositories, "") == Words{"overlay", "test_repo"});
    CHECK(complete(Completing::repositories, "t") == Words{"test_repo"});
}
