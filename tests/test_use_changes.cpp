#include "system_builder.hpp"
#include "use_changes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using egraph::test::make_system;

namespace {

std::vector<std::string> strings(const egraph::Evaluated& evaluated, egraph::Range range) {
    std::vector<std::string> found;
    for (const auto id : evaluated.ids_in(range)) {
        found.emplace_back(evaluated.string(id));
    }
    return found;
}

// "type parent atom matches..." per node.
std::vector<std::string> nodes(const egraph::Evaluated& evaluated, egraph::Range range) {
    std::vector<std::string> found;
    for (const auto& node : evaluated.nodes_in(range)) {
        auto line =
            std::format("{} {} {}", static_cast<int>(node.type),
                        node.parent == egraph::no_parent ? -1 : static_cast<int>(node.parent),
                        evaluated.string(node.atom));
        for (const auto id : evaluated.ids_in(node.matches)) {
            line += std::format(" {}", id);
        }
        found.push_back(std::move(line));
    }
    return found;
}

} // namespace

TEST_CASE("a USE change rebuilds the candidate's USE and dependencies") {
    const auto system = make_system(
        {{.cpv = "dev-libs/base-1"}},
        {{.cpv = "dev-libs/lib-2",
          .deps = {{"RDEPEND", "gtk? ( dev-libs/gtkdep ) dev-libs/base !qt? ( !dev-libs/base )"}},
          .iuse = "gtk qt",
          .use = "qt"},
         {.cpv = "dev-libs/gtkdep-1"}});
    const auto& [store, evaluated] = system;
    // Candidates sort by cp.
    REQUIRE(evaluated.string(evaluated.candidates.back().cpv) == "dev-libs/lib-2");
    const auto rdepend = egraph::dep_kinds.size() - 1;
    CHECK(nodes(evaluated, evaluated.candidates.back().deps.at(rdepend)) ==
          std::vector<std::string>{"0 -1 dev-libs/base 0"});

    const std::vector<egraph::UseChange> changes{
        {.candidate = 1, .flags = {{"gtk", true}, {"qt", false}}}};
    const auto changed = egraph::with_use_changes(evaluated, store, changes);
    const auto& lib = changed.candidates.back();
    CHECK(strings(changed, lib.use) == std::vector<std::string>{"gtk"});
    CHECK(nodes(changed, lib.deps.at(rdepend)) ==
          std::vector<std::string>{"0 -1 dev-libs/gtkdep", "0 -1 dev-libs/base 0",
                                   "3 -1 !dev-libs/base 0"});
    // Everything else as it was.
    CHECK(changed.candidates.size() == evaluated.candidates.size());
    CHECK(changed.candidates.front().deps.at(rdepend).count == 0);
    CHECK(changed.string(lib.cpv) == "dev-libs/lib-2");
}

TEST_CASE("no USE change leaves the candidate's dependencies as the builder stored them") {
    const auto system =
        make_system({{.cpv = "dev-libs/base-1"}},
                    {{.cpv = "dev-libs/lib-2",
                      .deps = {{"RDEPEND", "|| ( dev-libs/base dev-libs/other ) x? ( dev-libs/x )"},
                               {"DEPEND", "dev-libs/base"}},
                      .iuse = "x",
                      .use = "x"}});
    const auto& [store, evaluated] = system;
    const std::vector<egraph::UseChange> changes{{.candidate = 0, .flags = {}}};
    const auto changed = egraph::with_use_changes(evaluated, store, changes);
    for (std::size_t kind = 0; kind < egraph::dep_kinds.size(); ++kind) {
        CHECK(nodes(changed, changed.candidates.front().deps.at(kind)) ==
              nodes(evaluated, evaluated.candidates.front().deps.at(kind)));
    }
}
