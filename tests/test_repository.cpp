#include "index_builder.hpp"
#include "repository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::distfiles_size;
using egraph::test::IndexBuilder;

TEST_CASE("a version's distfiles are those of the groups its USE meets") {
    IndexBuilder builder;
    builder.version({.cpv = "app-misc/foo-1",
                     .fetch = {{.places = {""}, .bytes = 100},
                               {.places = {"doc?"}, .bytes = 20},
                               {.places = {"!doc?"}, .bytes = 7},
                               {.places = {"x86? l10n_de?"}, .bytes = 3},
                               {.places = {"x86? !l10n_de?"}, .bytes = 1},
                               {.places = {"de?", "de-AT?"}, .bytes = 50}}});
    builder.version({.cpv = "app-misc/bare-1"});
    const auto& index = builder.index();
    const auto& foo = index.versions.at(0);
    const auto size = [&](const std::vector<std::string>& use) {
        return distfiles_size(index, foo, use);
    };
    CHECK(size({}) == 107);
    CHECK(size({"doc"}) == 120);
    CHECK(size({"x86"}) == 108);
    CHECK(size({"doc", "l10n_de", "x86"}) == 123);
    CHECK(size({"l10n_de"}) == 107);
    // A file in two places counts once, either's conditionals met.
    CHECK(size({"de"}) == 157);
    CHECK(size({"de", "de-AT"}) == 157);
    CHECK(size({"de-AT"}) == 157);
    CHECK(distfiles_size(index, index.versions.at(1), std::vector<std::string>{"doc"}) == 0);
}
