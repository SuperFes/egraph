#include "graph.hpp"
#include "plan.hpp"
#include "remedy.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

using egraph::test::make_system;

namespace {

// "held: holder [dependents] [set atom], ...; nodeps; removable, frees held ...".
std::vector<std::string> remedies(const egraph::test::System& system,
                                  const egraph::Targets& targets = {},
                                  const egraph::Rescope& rescope = {}) {
    const auto& [store, evaluated] = system;
    const auto cpv = [&store](std::uint32_t id) {
        return std::string(store.string(store.packages.at(id).cpv));
    };
    const auto plan = egraph::plan_updates(store, evaluated, egraph::UseRebuilds::none, targets);
    const auto graph = egraph::build_graph(store);
    std::vector<std::string> lines;
    for (const auto& remedy : egraph::remedies(store, evaluated, graph, plan,
                                               egraph::UseRebuilds::none, targets, rescope)) {
        auto line = std::format("{}:", cpv(plan.held.at(remedy.held).package));
        for (const auto& holder : remedy.holders) {
            line += std::format(" {} [", cpv(holder.package));
            for (const auto dependent : holder.dependents) {
                line += std::format("{}{}", line.ends_with('[') ? "" : " ", cpv(dependent));
            }
            line += "] [";
            for (const auto root : holder.roots) {
                const auto& found = store.roots.at(root);
                line += std::format("{}{} {}", line.ends_with('[') ? "" : " ",
                                    store.string(found.set), store.string(found.atom));
            }
            line += ']';
        }
        if (remedy.nodeps) {
            line += "; nodeps";
        }
        if (remedy.removable) {
            line += "; removable";
            for (const auto freed : remedy.frees) {
                line += std::format(" frees {}", cpv(plan.held.at(freed).package));
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

TEST_CASE("a holder only world keeps can be removed for the update") {
    const auto system = make_system(
        {{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
         {.cpv = "dev-libs/astroid-4.0"}},
        {{.cpv = "dev-libs/astroid-4.0"}, {.cpv = "dev-libs/astroid-4.3"}}, {"app-misc/pylint"});
    CHECK(remedies(system) ==
          std::vector<std::string>{"dev-libs/astroid-4.0: app-misc/pylint-1 [] [selected "
                                   "app-misc/pylint]; nodeps; removable"});
    // Nothing keeping it at all is a leaf too.
    const auto orphan =
        make_system({{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
                     {.cpv = "dev-libs/astroid-4.0"}},
                    {{.cpv = "dev-libs/astroid-4.0"}, {.cpv = "dev-libs/astroid-4.3"}});
    CHECK(remedies(orphan) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0: app-misc/pylint-1 [] []; nodeps; removable"});
}

TEST_CASE("a holder something else needs, or another set keeps, is only named") {
    const std::vector<egraph::test::Installed> installed{
        {.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "media-libs/mesa"}}},
        {.cpv = "llvm/libclc-22"},
        {.cpv = "media-libs/mesa-1", .deps = {{"RDEPEND", "=llvm/libclc-22*"}}}};
    const std::vector<egraph::test::Available> available{{.cpv = "llvm/libclc-22"},
                                                         {.cpv = "llvm/libclc-23"}};
    CHECK(remedies(make_system(installed, available, {"app-misc/kwin", "media-libs/mesa"})) ==
          std::vector<std::string>{"llvm/libclc-22: media-libs/mesa-1 [app-misc/kwin-1] "
                                   "[selected media-libs/mesa]; nodeps"});
    CHECK(remedies(make_system({installed.at(1), installed.at(2)}, available, {},
                               {"media-libs/mesa"})) ==
          std::vector<std::string>{"llvm/libclc-22: media-libs/mesa-1 [] [system "
                                   "media-libs/mesa]; nodeps"});
}

TEST_CASE("removing holders frees every update they alone hold") {
    const auto system =
        make_system({{.cpv = "app-misc/pylint-1",
                      .deps = {{"RDEPEND", "<dev-libs/astroid-4.1 <dev-libs/lazy-2"}}},
                     {.cpv = "app-misc/plugin-1",
                      .deps = {{"RDEPEND", "app-misc/pylint <dev-libs/astroid-4.2"}}},
                     {.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "dev-libs/lazy-1"}},
                    {{.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "dev-libs/astroid-4.3"},
                     {.cpv = "dev-libs/lazy-1"},
                     {.cpv = "dev-libs/lazy-2"}},
                    {"app-misc/pylint", "app-misc/plugin"});
    // The plugin depends on pylint, which goes with it.
    CHECK(remedies(system) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0: app-misc/pylint-1 [] [selected app-misc/pylint] "
              "app-misc/plugin-1 [] [selected app-misc/plugin]; nodeps; removable frees "
              "dev-libs/lazy-1",
              "dev-libs/lazy-1: app-misc/pylint-1 [app-misc/plugin-1] [selected "
              "app-misc/pylint]; nodeps"});
}

TEST_CASE("a held update its own dependencies stop has no remedy") {
    const auto system =
        make_system({{.cpv = "dev-libs/astroid-4.0"}},
                    {{.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "dev-libs/astroid-4.3", .deps = {{"RDEPEND", "dev-libs/missing"}}}});
    CHECK(remedies(system) == std::vector<std::string>{"dev-libs/astroid-4.0:"});
}

TEST_CASE("a removal is offered only when the plan without the holders merges the update") {
    // The update also needs one another package holds, and a leaf holder behind that.
    const auto system =
        make_system({{.cpv = "app-misc/pylint-1", .deps = {{"RDEPEND", "<dev-libs/astroid-4.1"}}},
                     {.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "<dev-libs/lazy-2"}}},
                     {.cpv = "app-misc/panel-1", .deps = {{"RDEPEND", "app-misc/kwin"}}},
                     {.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "dev-libs/lazy-1"}},
                    {{.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "dev-libs/astroid-4.3", .deps = {{"RDEPEND", ">=dev-libs/lazy-2"}}},
                     {.cpv = "dev-libs/lazy-1"},
                     {.cpv = "dev-libs/lazy-2"}},
                    {"app-misc/pylint", "app-misc/panel"});
    CHECK(remedies(system) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0: app-misc/pylint-1 [] [selected app-misc/pylint]; nodeps",
              "dev-libs/lazy-1: app-misc/kwin-1 [app-misc/panel-1] []; nodeps"});
}

TEST_CASE("from the root sets, the scope without the holders is depclean's") {
    // Only pylint keeps lazy, whose update pylint also holds: removing pylint for astroid leaves
    // lazy orphaned rather than freed.
    const auto system = make_system(
        {{.cpv = "app-misc/pylint-1",
          .deps = {{"RDEPEND", "<dev-libs/astroid-4.1 dev-libs/lazy <dev-libs/lazy-2"}}},
         {.cpv = "app-misc/tool-1", .deps = {{"RDEPEND", "dev-libs/astroid"}}},
         {.cpv = "dev-libs/astroid-4.0"},
         {.cpv = "dev-libs/lazy-1"}},
        {{.cpv = "dev-libs/astroid-4.0"},
         {.cpv = "dev-libs/astroid-4.3"},
         {.cpv = "dev-libs/lazy-1"},
         {.cpv = "dev-libs/lazy-2"}},
        {"app-misc/pylint", "app-misc/tool"});
    const egraph::Targets world{.scope = egraph::keep(system.store, {}).packages, .roots = true};
    const egraph::Rescope rescope = [&system](const std::vector<bool>& removed) {
        return egraph::keep(system.store, {.build_deps = true, .masking = {}, .removed = removed})
            .packages;
    };
    CHECK(remedies(system, world, rescope) ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0: app-misc/pylint-1 [] [selected app-misc/pylint]; nodeps; "
              "removable",
              "dev-libs/lazy-1: app-misc/pylint-1 [] [selected app-misc/pylint]; nodeps; "
              "removable frees dev-libs/astroid-4.0"});
    // Without depclean's scope, lazy stays in and is freed along with astroid.
    CHECK(remedies(system, world).front().ends_with("removable frees dev-libs/lazy-1"));
}

TEST_CASE("a rebuild whose own ebuild rejects the update stands for its installed package") {
    const auto system = make_system(
        {{.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "app-misc/rgb:0/rc3="}}},
         {.cpv = "app-misc/rgb-1_rc3", .sub_slot = "rc3"}},
        {{.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<app-misc/rgb-1 app-misc/rgb:="}}},
         {.cpv = "app-misc/rgb-1_rc3", .sub_slot = "rc3"},
         {.cpv = "app-misc/rgb-1", .sub_slot = "1"}},
        {"app-misc/skin", "app-misc/rgb"});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none);
    REQUIRE(plan.held.size() == 1);
    CHECK(plan.held.front().reasons.front().member.candidate);
    CHECK(remedies(system) ==
          std::vector<std::string>{"app-misc/rgb-1_rc3: app-misc/skin-1 [] [selected "
                                   "app-misc/skin]; nodeps; removable"});
}

TEST_CASE("updates --held lists each held update's remedies under it") {
    const auto system =
        make_system({{.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "media-libs/mesa"}}},
                     {.cpv = "app-misc/pylint-1",
                      .deps = {{"RDEPEND", "<dev-libs/astroid-4.1 =llvm/libclc-22*"}}},
                     {.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "llvm/libclc-22"},
                     {.cpv = "media-libs/mesa-1", .deps = {{"RDEPEND", "=llvm/libclc-22*"}}}},
                    {{.cpv = "dev-libs/astroid-4.0"},
                     {.cpv = "dev-libs/astroid-4.3"},
                     {.cpv = "llvm/libclc-22"},
                     {.cpv = "llvm/libclc-23"}},
                    {"app-misc/kwin", "app-misc/pylint"});
    const auto& [store, evaluated] = system;
    const auto graph = egraph::build_graph(store);
    const auto lines =
        egraph::update_lines(store, evaluated, egraph::UseRebuilds::none, true, false, {},
                             egraph::RemedyInputs{.graph = graph, .rescope = {}});
    CHECK(lines ==
          std::vector<std::string>{
              "dev-libs/astroid-4.0\theld\tdev-libs/astroid-4.3\ttest_repo\t\tapp-misc/pylint-1 "
              "<dev-libs/astroid-4.1",
              "dev-libs/astroid-4.0\tholder\tapp-misc/pylint-1\t\t@selected app-misc/pylint",
              "dev-libs/astroid-4.0\tremove\t", "dev-libs/astroid-4.0\tnodeps",
              "llvm/libclc-22\theld\tllvm/libclc-23\ttest_repo\t\tapp-misc/pylint-1 "
              "=llvm/libclc-22*\tmedia-libs/mesa-1 =llvm/libclc-22*",
              "llvm/libclc-22\tholder\tapp-misc/pylint-1\t\t@selected app-misc/pylint",
              "llvm/libclc-22\tholder\tmedia-libs/mesa-1\tapp-misc/kwin-1",
              "llvm/libclc-22\tnodeps"});
    // In table form, led by two empty fields like the held line.
    const auto table =
        egraph::update_lines(store, evaluated, egraph::UseRebuilds::none, true, true, {},
                             egraph::RemedyInputs{.graph = graph, .rescope = {}});
    CHECK(table.size() == lines.size());
    CHECK(table.at(1) == "\t\t" + lines.at(1));
}
