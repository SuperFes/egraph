#include "exec.hpp"
#include "helpers.hpp"
#include "plan.hpp"
#include "query.hpp"
#include "replace.hpp"
#include "system_builder.hpp"
#include "verify.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

using egraph::test::Available;
using egraph::test::Installed;
using egraph::test::make_system;
using Json = nlohmann::json;

namespace {

std::vector<egraph::Atom> listed(const std::vector<std::string>& atoms) {
    std::vector<egraph::Atom> found;
    for (const auto& text : atoms) {
        found.push_back(egraph::parse_atom(text).value());
    }
    return found;
}

// The installed packages the plan uninstalls as replaced slots, each with the merges it goes
// after.
std::vector<std::string> replaced(const egraph::test::System& system, const egraph::Plan& plan) {
    std::vector<std::string> found;
    for (const auto& each : plan.uninstalls) {
        if (each.why) {
            continue;
        }
        auto line = std::string{system.store.string(system.store.packages.at(each.package).cpv)};
        for (const auto merge : each.after) {
            line += " after " +
                    std::string{system.evaluated.string(
                        system.evaluated.candidates.at(plan.merges.at(merge).candidate).cpv)};
        }
        found.push_back(std::move(line));
    }
    return found;
}

egraph::Targets world(const std::vector<std::string>& atoms) {
    return {.scope = {}, .roots = true, .replace_slots = listed(atoms)};
}

std::vector<std::string> replaced(const egraph::test::System& system,
                                  const egraph::Targets& targets) {
    return replaced(system, egraph::plan_updates(system.store, system.evaluated,
                                                 egraph::UseRebuilds::none, targets));
}

std::vector<Available> wines() {
    return {{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"},
            {.cpv = "app-emulation/wine-vanilla-9.0", .slot = "9.0"}};
}

} // namespace

TEST_CASE("the replace-slots list is an atom a line, comments and blank lines skipped") {
    const auto found =
        egraph::parse_replace_slots("# slots to replace\n\napp-emulation/wine-vanilla\n  "
                                    "<sys-kernel/gentoo-sources-7  # old\n");
    REQUIRE(found);
    REQUIRE(found->size() == 2);
    CHECK(found->at(0).cp == "app-emulation/wine-vanilla");
    CHECK(found->at(1).cp == "sys-kernel/gentoo-sources");
    CHECK(found->at(1).op == egraph::Operator::less);
    CHECK(egraph::parse_replace_slots("")->empty());
    const auto bad = egraph::parse_replace_slots("app-misc/foo\n\n!app-misc/bar\n");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().starts_with("line 3: "));
}

TEST_CASE("the replace-slots list lives under the configuration root, none when it is missing") {
    const egraph::test::TempDir root;
    CHECK(egraph::replace_slots_path(root.path()) == root.path() / "etc/egraph/replace-slots");
    CHECK(egraph::read_replace_slots(egraph::replace_slots_path(root.path()))->empty());
    std::filesystem::create_directories(root.path() / "etc/egraph");
    egraph::test::write_text(root.path() / "etc/egraph/replace-slots",
                             "app-misc/foo\nnot an atom\n");
    const auto bad = egraph::read_replace_slots(egraph::replace_slots_path(root.path()));
    REQUIRE_FALSE(bad);
    CHECK(bad.error().starts_with((root.path() / "etc/egraph/replace-slots").string() +
                                  ": line 2: "));
    egraph::test::write_text(root.path() / "etc/egraph/replace-slots", "app-misc/foo\n");
    const auto found = egraph::read_replace_slots(egraph::replace_slots_path(root.path()));
    REQUIRE(found);
    REQUIRE(found->size() == 1);
    CHECK(found->front().cp == "app-misc/foo");
}

TEST_CASE("a listed package's old slot goes once the root atom's new slot merges") {
    const auto system = make_system({{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"}},
                                    wines(), {"app-emulation/wine-vanilla"});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none,
                             world({"app-emulation/wine-vanilla"}));
    CHECK(replaced(system, plan) == std::vector<std::string>{"app-emulation/wine-vanilla-8.0 after "
                                                             "app-emulation/wine-vanilla-9.0"});
    CHECK(egraph::update_lines(system.store, system.evaluated, plan, egraph::UseRebuilds::none,
                               false, false, world({"app-emulation/wine-vanilla"})) ==
          std::vector<std::string>{
              "app-emulation/wine-vanilla-9.0\tnew-slot\tapp-emulation/wine-vanilla-9.0\ttest_repo"
              "\t\t@selected app-emulation/wine-vanilla\tapp-emulation/wine-vanilla-8.0:8.0",
              "app-emulation/wine-vanilla-8.0\tuninstall\t\t\tapp-emulation/wine-vanilla-9.0"});
    // emerge leaves it to its depclean.
    CHECK(egraph::planned_merges(system.store, system.evaluated, plan).merges.size() == 1);
    // The default is emerge's: both slots stay.
    CHECK(replaced(system, world({})).empty());
    CHECK(replaced(system, world({"app-misc/other"})).empty());
    // The list's atom picks the slots that go.
    CHECK(replaced(system, world({"<app-emulation/wine-vanilla-8"})).empty());
    CHECK(replaced(system, world({"app-emulation/wine-vanilla:8.0"})).size() == 1);
}

TEST_CASE("every old slot a listed root atom leaves goes, each after the merge replacing it") {
    const auto system = make_system({{.cpv = "app-emulation/wine-vanilla-7.0", .slot = "7.0"},
                                     {.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"}},
                                    {{.cpv = "app-emulation/wine-vanilla-7.0", .slot = "7.0"},
                                     {.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"},
                                     {.cpv = "app-emulation/wine-vanilla-9.0", .slot = "9.0"}},
                                    {"app-emulation/wine-vanilla"});
    CHECK(replaced(system, world({"app-emulation/wine-vanilla"})) ==
          std::vector<std::string>{
              "app-emulation/wine-vanilla-7.0 after app-emulation/wine-vanilla-9.0",
              "app-emulation/wine-vanilla-8.0 after app-emulation/wine-vanilla-9.0"});
}

TEST_CASE("an old slot something still needs stays") {
    const std::vector<std::string> list{"app-emulation/wine-vanilla"};
    const Installed old{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"};
    auto available = wines();
    available.push_back({.cpv = "app-misc/user-1"});
    available.push_back({.cpv = "app-misc/other-1"});
    const auto with_user = [&](const std::string& rdepend,
                               const std::vector<Installed>& more = {}) {
        std::vector<Installed> installed{
            old, {.cpv = "app-misc/user-1", .deps = {{"RDEPEND", rdepend}}}};
        installed.insert(installed.end(), more.begin(), more.end());
        return make_system(installed, available, {"app-emulation/wine-vanilla", "app-misc/user"});
    };
    // Pinned to its slot, at run time or build time.
    CHECK(replaced(with_user("app-emulation/wine-vanilla:8.0"), world(list)).empty());
    CHECK(replaced(make_system({old,
                                {.cpv = "app-misc/user-1",
                                 .deps = {{"BDEPEND", "app-emulation/wine-vanilla:8.0"}}}},
                               available, {"app-emulation/wine-vanilla", "app-misc/user"}),
                   world(list))
              .empty());
    // Any slot does: the new one satisfies it.
    CHECK(replaced(with_user("app-emulation/wine-vanilla"), world(list)).size() == 1);
    // An alternative something else left installed satisfies does not need it.
    CHECK(replaced(with_user("|| ( app-emulation/wine-vanilla:8.0 app-misc/other )"), world(list))
              .empty());
    CHECK(replaced(with_user("|| ( app-emulation/wine-vanilla:8.0 app-misc/other )",
                             {{.cpv = "app-misc/other-1"}}),
                   world(list))
              .size() == 1);
    // A root atom naming its slot.
    CHECK(replaced(make_system({old}, wines(),
                               {"app-emulation/wine-vanilla", "app-emulation/wine-vanilla:8.0"}),
                   world(list))
              .empty());
    // A merge's dependency.
    auto pinning = wines();
    pinning.at(1).deps = {{"RDEPEND", "app-emulation/wine-vanilla:8.0"}};
    CHECK(
        replaced(make_system({old}, pinning, {"app-emulation/wine-vanilla"}), world(list)).empty());
}

TEST_CASE("only a root atom's new slot replaces an old one") {
    const std::vector<std::string> list{"app-emulation/wine-vanilla"};
    auto available = wines();
    available.push_back(
        {.cpv = "app-misc/user-2", .deps = {{"RDEPEND", "app-emulation/wine-vanilla:9.0"}}});
    available.push_back({.cpv = "app-misc/user-1"});
    // A dependency pulls the new slot in; the old one is a root atom's pinned slot.
    const auto system = make_system(
        {{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"}, {.cpv = "app-misc/user-1"}},
        available, {"app-emulation/wine-vanilla:8.0", "app-misc/user"});
    CHECK(replaced(system, world(list)).empty());
    // Not in the world file at all.
    const auto unrooted = make_system(
        {{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"}, {.cpv = "app-misc/user-1"}},
        available, {"app-misc/user"});
    CHECK(replaced(unrooted, world(list)).empty());
}

TEST_CASE("an old slot a blocker already uninstalls is not replaced as well") {
    auto available = wines();
    available.at(1).deps = {{"RDEPEND", "!app-emulation/wine-vanilla:8.0"}};
    const auto system = make_system({{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"}},
                                    available, {"app-emulation/wine-vanilla"});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none,
                             world({"app-emulation/wine-vanilla"}));
    REQUIRE(plan.uninstalls.size() == 1);
    CHECK(plan.uninstalls.front().why.has_value());
}

TEST_CASE("a replaced slot's uninstall leaves the world file alone") {
    const auto system = make_system({{.cpv = "app-emulation/wine-vanilla-8.0", .slot = "8.0"}},
                                    wines(), {"app-emulation/wine-vanilla"});
    const std::vector<egraph::Argument> arguments{
        {.set = "", .atom = "app-emulation/wine-vanilla"}};
    auto targets = world({"app-emulation/wine-vanilla"});
    targets.request = arguments;
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none, targets);
    REQUIRE(replaced(system, plan).size() == 1);
    std::vector<Json> requests;
    for (const auto& step :
         egraph::run_steps(system.store, system.evaluated, plan, arguments, false)) {
        requests.push_back(
            Json::parse(egraph::worker_request(system.store, system.evaluated, plan, step)));
    }
    REQUIRE(requests.size() == 2);
    CHECK(requests.at(1) == Json::parse(R"({"uninstall": "app-emulation/wine-vanilla-8.0"})"));
}

TEST_CASE("the running kernel's release is proc's osrelease") {
    const egraph::test::TempDir proc;
    CHECK(egraph::kernel_release(proc.path()) == std::nullopt);
    std::filesystem::create_directories(proc.path() / "sys/kernel");
    egraph::test::write_text(proc.path() / "sys/kernel/osrelease", "7.2.8-gentoo-x86_64\n");
    CHECK(egraph::kernel_release(proc.path()) == "7.2.8-gentoo-x86_64");
}

TEST_CASE("a kernel's sources are where its modules' build link points") {
    const egraph::test::TempDir root;
    const auto modules = root.path() / "lib/modules";
    CHECK(egraph::kernel_sources(root.path(), "1-gentoo") == std::nullopt);
    std::filesystem::create_directories(modules / "1-gentoo");
    std::filesystem::create_symlink("/usr/src/linux-1-gentoo", modules / "1-gentoo/build");
    CHECK(egraph::kernel_sources(root.path(), "1-gentoo") == "/usr/src/linux-1-gentoo");
    // A relative one, as a distribution kernel's, from where the link is on that system.
    std::filesystem::create_directories(modules / "2-dist");
    std::filesystem::create_symlink("../../../usr/src/linux-2-dist", modules / "2-dist/build");
    CHECK(egraph::kernel_sources(root.path(), "2-dist") == "/usr/src/linux-2-dist");
    // Without build, source.
    std::filesystem::create_directories(modules / "3-gentoo");
    std::filesystem::create_symlink("/usr/src/linux-3-gentoo/", modules / "3-gentoo/source");
    CHECK(egraph::kernel_sources(root.path(), "3-gentoo") == "/usr/src/linux-3-gentoo");
    // Modules without either.
    std::filesystem::create_directories(modules / "4-gentoo");
    CHECK(egraph::kernel_sources(root.path(), "4-gentoo") == std::nullopt);
}

TEST_CASE("egraph-build's kernel sources are read back by cpv") {
    const auto found = egraph::parse_kernel_sources(
        R"({"sys-kernel/sources-1": ["/usr/src/linux-1-gentoo"], "app-misc/other-1": []})");
    REQUIRE(found);
    CHECK(*found == egraph::KernelSources{{"app-misc/other-1", {}},
                                          {"sys-kernel/sources-1", {"/usr/src/linux-1-gentoo"}}});
    CHECK_FALSE(egraph::parse_kernel_sources("not json"));
    CHECK_FALSE(egraph::parse_kernel_sources(R"({"x/y-1": "/usr/src/linux-1"})"));
    CHECK_FALSE(egraph::parse_kernel_sources(R"(["x/y-1"])"));
}

TEST_CASE("the running kernel's sources keep their slot") {
    const auto system = make_system({{.cpv = "sys-kernel/sources-1", .slot = "1"},
                                     {.cpv = "sys-kernel/sources-2", .slot = "2"}},
                                    {{.cpv = "sys-kernel/sources-1", .slot = "1"},
                                     {.cpv = "sys-kernel/sources-2", .slot = "2"},
                                     {.cpv = "sys-kernel/sources-3", .slot = "3"}},
                                    {"sys-kernel/sources"});
    auto targets = world({"sys-kernel/sources"});
    const auto plan =
        egraph::plan_updates(system.store, system.evaluated, egraph::UseRebuilds::none, targets);
    CHECK(egraph::replaced_slots(plan) == std::vector<std::uint32_t>{0, 1});
    const egraph::KernelSources sources{{"sys-kernel/sources-1", {"/usr/src/linux-1-gentoo"}},
                                        {"sys-kernel/sources-2", {"/usr/src/linux-2-gentoo"}}};
    const auto owners = egraph::running_kernel_owners(system.store, egraph::replaced_slots(plan),
                                                      sources, "/usr/src/linux-2-gentoo");
    CHECK(owners == std::vector<std::uint32_t>{1});
    CHECK(egraph::running_kernel_owners(system.store, egraph::replaced_slots(plan), sources,
                                        "/usr/src/linux-9-gentoo")
              .empty());
    // A package egraph-build said nothing of is kept: it cannot be shown not to own them.
    CHECK(egraph::running_kernel_owners(system.store, egraph::replaced_slots(plan), {},
                                        "/usr/src/linux-2-gentoo") ==
          std::vector<std::uint32_t>{0, 1});
    targets.kept_slots = owners;
    CHECK(replaced(system, targets) ==
          std::vector<std::string>{"sys-kernel/sources-1 after sys-kernel/sources-3"});
}
