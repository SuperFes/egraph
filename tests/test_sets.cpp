#include "sets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using egraph::Argument;

TEST_CASE("egraph-build's set list is read with each set's atoms or why it cannot be loaded") {
    const auto list = egraph::parse_sets(R"({
        "sets": [
            {"name": "desktop", "class": "StaticFileSet", "description": "from a file",
             "world_candidate": true,
             "atoms": [["desktop", "app-misc/viewer"], ["tools", "dev-util/tool"]]},
            {"name": "broken", "class": "StaticFileSet", "description": "",
             "world_candidate": false, "error": "it names a set that does not exist: x"},
            {"name": "nope", "error": "no such set"},
            {"name": "live-rebuild", "class": "VariableSet", "description": "by PROPERTIES",
             "world_candidate": false}
        ],
        "errors": ["sets.conf: no class"]
    })");
    REQUIRE(list);
    REQUIRE(list->sets.size() == 4);
    const auto& desktop = list->sets.at(0);
    CHECK(desktop.name == "desktop");
    CHECK(desktop.kind == "StaticFileSet");
    CHECK(desktop.description == "from a file");
    CHECK(desktop.world_candidate);
    CHECK(desktop.atoms == std::vector<Argument>{{.set = "desktop", .atom = "app-misc/viewer"},
                                                 {.set = "tools", .atom = "dev-util/tool"}});
    CHECK_FALSE(desktop.error);
    CHECK(list->sets.at(1).error == "it names a set that does not exist: x");
    CHECK_FALSE(list->sets.at(1).atoms);
    CHECK(list->sets.at(2).kind.empty());
    CHECK(list->sets.at(2).error == "no such set");
    CHECK_FALSE(list->sets.at(3).atoms);
    CHECK_FALSE(list->sets.at(3).error);
    CHECK(list->errors == std::vector<std::string>{"sets.conf: no class"});
}

TEST_CASE("a set list that is not egraph-build's is refused") {
    CHECK(egraph::parse_sets("[]").error() == "not a JSON object");
    CHECK(egraph::parse_sets("{").error() == "not a JSON object");
    CHECK(egraph::parse_sets(R"({"sets": [{"name": "a"}]})").error() ==
          "errors: not a list of strings");
    CHECK(egraph::parse_sets(R"({"sets": [{"class": "A"}], "errors": []})").error() ==
          "sets: not a list of named sets");
    CHECK(egraph::parse_sets(R"({"sets": [{"name": "a", "atoms": [["a"]]}], "errors": []})")
              .error() == "sets: a's atoms are not (set, atom) pairs");
    CHECK(egraph::parse_sets(R"({"sets": [{"name": "a", "world_candidate": 1}], "errors": []})")
              .error() == "sets: a's world_candidate is not true or false");
}

TEST_CASE("the sets loaded give their atoms by name, or the first that could not be loaded") {
    egraph::SetList list;
    list.sets.push_back({.name = "desktop",
                         .kind = "StaticFileSet",
                         .description = "",
                         .world_candidate = true,
                         .atoms = std::vector<Argument>{{.set = "tools", .atom = "dev-util/tool"}},
                         .error = std::nullopt});
    list.sets.push_back({.name = "empty",
                         .kind = "PreservedLibraryConsumerSet",
                         .description = "",
                         .world_candidate = false,
                         .atoms = std::vector<Argument>{},
                         .error = std::nullopt});
    const auto loaded = egraph::loaded_sets(list);
    REQUIRE(loaded);
    CHECK(*loaded == egraph::Sets{{"desktop",
                                   {.atoms = {{.set = "tools", .atom = "dev-util/tool"}},
                                    .world_candidate = true}},
                                  {"empty", {.atoms = {}, .world_candidate = false}}});
    list.sets.push_back({.name = "nope",
                         .kind = "",
                         .description = "",
                         .world_candidate = false,
                         .atoms = std::nullopt,
                         .error = "no such set"});
    list.sets.push_back({.name = "broken",
                         .kind = "StaticFileSet",
                         .description = "",
                         .world_candidate = false,
                         .atoms = std::nullopt,
                         .error = "it names a set that does not exist: x"});
    CHECK(egraph::loaded_sets(list).error() == "@nope: no such set");
}
