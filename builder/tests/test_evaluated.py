"""The evaluated layer against the oracle on every scenario, and by hand on two of them."""

import collections

import pytest
from portage import best
from portage.exception import InvalidAtom, InvalidDependString
from portage.versions import cpv_getkey

import compare
import update
from scenarios import SCENARIOS
from egraph_build import evaluated, oracle
from egraph_build.model import DEP_KINDS, Edge

_built = {}


def portdb(system):
    return system.trees[system.eroot]["porttree"].dbapi


def build(system):
    if system.eroot not in _built:
        _built[system.eroot] = evaluated.build(system.vardb, portdb(system))
    return _built[system.eroot]


def installed_cps(system):
    return sorted({cpv_getkey(cpv) for cpv in oracle.installed(system.vardb)})


def test_every_installed_package_is_evaluated(scenario):
    assert build(scenario).installed() == oracle.installed(scenario.vardb)


def test_dependencies_follow_the_oracle(scenario):
    layer = build(scenario)
    for cpv in oracle.installed(scenario.vardb):
        pkg = layer.package(cpv)
        source, _, eapi = oracle.dynamic_dep_strings(
            scenario.vardb, portdb(scenario), cpv
        )
        assert (evaluated.SOURCES[pkg.source], pkg.eapi) == (source, eapi), cpv
        assert layer.deps(cpv) == oracle.dynamic_deps(
            scenario.vardb, portdb(scenario), cpv
        ), cpv


def test_unparsable_dependencies_are_errors(scenario):
    layer = build(scenario)
    for cpv in oracle.installed(scenario.vardb):
        failing = set()
        for kind in DEP_KINDS:
            try:
                oracle.dynamic_dep_atoms(scenario.vardb, portdb(scenario), cpv, kind)
            except (InvalidAtom, InvalidDependString):
                failing.add(kind)
        assert {kind for kind, _ in layer.package(cpv).errors} == failing, cpv


def test_visible_candidates_are_the_visible_versions(scenario):
    layer = build(scenario)
    for cp in installed_cps(scenario):
        visible = {c.cpv for c in layer.candidates(cp) if not c.reasons}
        assert visible == set(portdb(scenario).xmatch("match-visible", cp)), cp


def test_masked_candidates_are_installed_and_say_why(scenario):
    layer = build(scenario)
    installed = set(oracle.installed(scenario.vardb))
    for c in layer.candidates():
        if c.reasons:
            assert c.cpv in installed, c
            assert c.reasons == oracle.mask_reasons(portdb(scenario), c.cpv, c.repo)
    for cpv in installed:
        for repo in portdb(scenario).getRepositories():
            if not portdb(scenario).cpv_exists(cpv, myrepo=repo):
                continue
            reasons = oracle.mask_reasons(portdb(scenario), cpv, repo)
            found = [c for c in layer.candidates(cpv_getkey(cpv)) if c.cpv == cpv]
            assert [(c.repo, c.reasons) for c in found if c.repo == repo] == [
                (repo, reasons)
            ], cpv


def test_candidate_use_is_the_effective_use(scenario):
    for c in build(scenario).candidates():
        (default_repo,) = portdb(scenario).aux_get(c.cpv, ["repository"])
        if c.repo == default_repo:
            assert c.use == oracle.effective_use(portdb(scenario), c.cpv), c


def test_best_visible_per_slot(scenario):
    layer = build(scenario)
    for cp in installed_cps(scenario):
        by_slot = collections.defaultdict(list)
        for c in layer.candidates(cp):
            if not c.reasons:
                by_slot[c.slot].append(c.cpv)
        found = {slot: best(cpvs) for slot, cpvs in by_slot.items()}
        assert found == oracle.best_visible(portdb(scenario), cp), cp


def test_candidates_are_sorted_and_unique(scenario):
    candidates = build(scenario).candidates()
    keys = [(c.cp, c.cpv, c.repo) for c in candidates]
    assert keys == sorted(set(keys))


def test_candidate_deps_are_depgraphs(scenario):
    for c in build(scenario).candidates():
        if c.reasons:
            assert c.deps == evaluated.NO_DEPS, c
            continue
        found = {
            kind: frozenset(node.atom for node in nodes if node.atom)
            for kind, nodes in zip(DEP_KINDS, c.deps)
        }
        assert found == update.candidate_deps(scenario.trees, scenario.eroot, c), c


def test_candidates_reach_what_nothing_installed_satisfies(scenario):
    """Every cp an unsatisfied dependency names has its candidates, when it has any ebuild, and
    every candidate cp is installed or so named."""
    layer = build(scenario)
    db = portdb(scenario)
    trees = [pkg.deps for pkg in layer]
    trees.extend(c.deps for c in layer.candidates() if not c.reasons)
    named = set().union(*map(evaluated.unsatisfied_cps, trees))
    present = {c.cp for c in layer.candidates()}
    for cp in named:
        if db.cp_list(cp):
            assert cp in present, cp
    assert present <= set(installed_cps(scenario)) | named


def test_candidates_follow_new_packages(playgrounds):
    layer = build(playgrounds("pulls"))
    present = {c.cp for c in layer.candidates()}
    assert present - set(installed_cps(playgrounds("pulls"))) == {
        "dev-cpp/mm-common",
        "dev-libs/chain",
        "dev-libs/extra",
        "dev-libs/first",
        "dev-libs/fresh",
        "dev-libs/second",
    }
    (glibmm,) = [c for c in layer.candidates("app-misc/glibmm") if c.cpv.endswith("-2")]
    (node,) = glibmm.deps[DEP_KINDS.index("BDEPEND")]
    assert (node.atom, node.matches) == ("dev-cpp/mm-common", ())
    (flagged,) = [
        c for c in layer.candidates("app-misc/flagged") if c.cpv.endswith("-2")
    ]
    assert [n.atom for n in flagged.deps[DEP_KINDS.index("RDEPEND")]] == [
        "dev-libs/extra"
    ]


# By hand, on the repository scenario.


@pytest.fixture
def repository(playgrounds):
    return build(playgrounds("repository"))


@pytest.mark.parametrize(
    "cpv, source",
    [
        ("app-misc/dyn-1", "ebuild"),
        ("app-misc/slotop-1", "ebuild"),
        ("app-misc/gone-1", "moved"),
        ("dev-libs/old-1", "vdb"),
    ],
)
def test_sources(repository, cpv, source):
    assert evaluated.SOURCES[repository.package(cpv).source] == source


def test_trees_hold_the_ebuilds_atoms(repository):
    rdepend = repository.package("app-misc/slotop-1").deps[DEP_KINDS.index("RDEPEND")]
    assert [(node.atom, node.matches) for node in rdepend] == [
        ("dev-libs/lib:=", ("dev-libs/lib-1", "dev-libs/lib-2")),
        ("dev-libs/lib:1/1=", ("dev-libs/lib-1",)),
    ]


def candidates(layer, cp):
    return [(c.cpv, c.slot, c.sub_slot, c.reasons) for c in layer.candidates(cp)]


def test_candidates(repository):
    # testing-2 is masked and not installed, so it is no candidate.
    assert candidates(repository, "app-misc/testing") == [
        ("app-misc/testing-1", "0", "0", ())
    ]
    assert candidates(repository, "app-misc/masked") == [
        ("app-misc/masked-1", "0", "0", ()),
        ("app-misc/masked-2", "0", "0", ("package.mask",)),
    ]
    assert candidates(repository, "app-misc/eula") == [
        ("app-misc/eula-1", "0", "0", ("EULA license(s)",))
    ]
    assert candidates(repository, "dev-libs/lib") == [
        ("dev-libs/lib-1", "1", "1", ()),
        ("dev-libs/lib-2", "2", "2", ()),
        ("dev-libs/lib-2.1", "2", "2.1", ()),
    ]
    # Gone from the repository.
    assert candidates(repository, "app-misc/gone") == []


def test_every_repository_has_its_candidates(repository):
    assert [(c.cpv, c.repo) for c in repository.candidates("dev-libs/new")] == [
        ("dev-libs/new-1", "overlay"),
        ("dev-libs/new-1", "test_repo"),
    ]
    assert [(c.cpv, c.repo) for c in repository.candidates("app-misc/over")] == [
        ("app-misc/over-1", "overlay"),
        ("app-misc/over-2", "overlay"),
    ]


def test_candidate_flags(repository):
    (flags,) = repository.candidates("app-misc/flags")
    assert (flags.use, flags.iuse) == (("new",), ("new", "old"))


def layer_possible(layer, cpv):
    return {
        (Edge(cpv, child, p.kind, p.atom, p.choice), p.flags)
        for p in layer.package(cpv).possible
        for child in p.matches
    }


def test_possible_dependencies_follow_the_oracle(scenario):
    layer = build(scenario)
    for cpv in oracle.installed(scenario.vardb):
        found = layer_possible(layer, cpv)
        deps = oracle.dynamic_deps(scenario.vardb, portdb(scenario), cpv)
        assert (
            compare.possible_mismatches(
                scenario.vardb, portdb(scenario), cpv, found, deps
            )
            == []
        ), cpv


def test_possible_dependencies_by_hand(playgrounds):
    layer = build(playgrounds("possible"))
    found = {
        (p.kind, p.atom, p.choice, p.matches, p.flags)
        for p in layer.package("app-misc/host-1").possible
    }

    def entry(atom, flags, choice=False, kind="RDEPEND", installed=True):
        name = atom.split("[")[0]
        return (kind, atom, choice, (f"{name}-1",) if installed else (), flags)

    assert found == {
        entry("dev-libs/x", ("a",)),
        entry("dev-libs/y", ("a", "b")),
        entry("dev-libs/deep", ("a", "b", "c")),
        entry("dev-libs/z", ("-minimal",)),
        entry("dev-libs/v", ("a",)),
        entry("dev-libs/w2", ("a",), choice=True),
        entry("dev-libs/v2", ("a",), choice=True),
        entry("dev-libs/absent", ("b",), installed=False),
        entry("dev-libs/lib[doc]", ("doc",)),
        entry("dev-libs/x", ("doc",), kind="DEPEND"),
    }
    # The vdb's strings are reduced already.
    assert all(not pkg.possible for pkg in layer if pkg.source != evaluated.EBUILD)


# Updates, held to emerge -u @installed.

USE_MODES = [(False, False), (True, False), (False, True)]


@pytest.mark.parametrize(
    "newuse, changed_use", USE_MODES, ids=["update", "newuse", "changed-use"]
)
def test_updates_follow_emerge(request, scenario, newuse, changed_use):
    """Where no installed dependent holds an update back: the targets are the best visible
    versions, and holds are the queries' (test_queries.test_updates_are_emerges)."""
    if SCENARIOS[request.node.callspec.params["scenario"]].get("bounded"):
        pytest.skip("installed dependents hold updates back here")
    if SCENARIOS[request.node.callspec.params["scenario"]].get("pulls"):
        pytest.skip("targets pull in new packages here")
    found = update.updates(scenario.trees, scenario.eroot, newuse, changed_use)
    if not found.success:
        pytest.skip("emerge cannot resolve @installed here")
    assert compare.layer_updates(build(scenario), newuse, changed_use) == found.replaced


def weighed(layer):
    """The packages whose masks the layer holds: not visible, or beside another installed
    version of their cp."""
    versions = collections.Counter(cpv_getkey(pkg.cpv) for pkg in layer)
    return {
        pkg.cpv for pkg in layer if not pkg.visible or versions[cpv_getkey(pkg.cpv)] > 1
    }


def test_masked_is_emerges(scenario, dynamic_deps):
    layer = build(scenario)
    found = {pkg.cpv: pkg.masked if dynamic_deps else pkg.vdb_masked for pkg in layer}
    expected = update.masked(scenario.trees, scenario.eroot, dynamic_deps)
    for cpv in found:
        assert found[cpv] == (expected[cpv] if cpv in weighed(layer) else False), cpv


@pytest.mark.parametrize(
    "cpv, masked, vdb_masked, visible",
    [
        ("dev-libs/gone-1", True, True, False),
        # Visible, and alone in its cp: depclean never weighs its masks.
        ("dev-libs/stale-1", False, False, True),
        ("dev-libs/kept-1", False, False, False),
        ("dev-libs/multi-2", True, True, False),
    ],
)
def test_masked_installed_packages(playgrounds, cpv, masked, vdb_masked, visible):
    pkg = build(playgrounds("masked-installed")).package(cpv)
    assert (pkg.masked, pkg.vdb_masked, pkg.visible) == (masked, vdb_masked, visible)


@pytest.mark.parametrize(
    "cpv", ["app-misc/bad-1", "app-misc/nocategory-1"], ids=["dependency", "soname"]
)
def test_unparsable_metadata_masks(playgrounds, cpv):
    name = "reference" if cpv == "app-misc/bad-1" else "sonames"
    pkg = build(playgrounds(name)).package(cpv)
    assert (pkg.masked, pkg.vdb_masked) == (True, True)


def test_visible_is_emerges(scenario):
    assert {pkg.cpv: pkg.visible for pkg in build(scenario)} == update.equiv_visible(
        scenario.trees, scenario.eroot
    )


@pytest.mark.parametrize(
    "cpv, visible, target, rebuild",
    [
        ("app-misc/up-1", True, ("app-misc/up-2", "test_repo"), ()),
        ("app-misc/rev-1", True, ("app-misc/rev-1-r1", "test_repo"), ()),
        ("dev-libs/slotted-1", True, ("dev-libs/slotted-1.1", "test_repo"), ()),
        ("dev-libs/slotted-2", True, None, ()),
        ("app-misc/testing-1", True, None, ()),
        ("app-misc/past-2", False, ("app-misc/past-3", "test_repo"), ()),
        ("app-misc/down-2", False, ("app-misc/down-1", "test_repo"), ()),
        ("app-misc/gone-2", False, ("app-misc/gone-1", "test_repo"), ()),
        ("app-misc/moved-1", True, None, ()),
        ("app-misc/twin-1", True, ("app-misc/twin-1", "overlay"), ("extra%*",)),
        ("app-misc/both-1", True, ("app-misc/both-2", "test_repo"), ()),
        (
            "app-misc/use-1",
            True,
            ("app-misc/use-1", "test_repo"),
            (
                "(-gone_off%)",
                "(-gone_on%*)",
                "-new_off%",
                "new_on%*",
                "-turned_off*",
                "turned_on*",
            ),
        ),
        (
            "app-misc/iuse-1",
            True,
            ("app-misc/iuse-1", "test_repo"),
            ("(-gone_off%)", "-new_off%"),
        ),
    ],
)
def test_updates_by_hand(playgrounds, cpv, visible, target, rebuild):
    pkg = build(playgrounds("updates")).package(cpv)
    assert (pkg.visible, pkg.target, pkg.rebuild) == (visible, target, rebuild)


def test_a_changed_default_rebuilds(repository):
    pkg = repository.package("app-misc/flags-1")
    assert pkg.target == ("app-misc/flags-1", "test_repo")
    assert pkg.rebuild == ("new*", "-old*")


@pytest.mark.parametrize(
    "old_use, old_iuse, use, iuse, forced, flags",
    [
        ("", "", "", "", "", ()),
        ("a", "a", "a", "a", "", ()),
        ("", "a", "a", "a", "", ("a*",)),
        ("a", "a", "", "a", "", ("-a*",)),
        ("", "", "a", "a", "", ("a%*",)),
        ("", "", "", "a", "", ("-a%",)),
        ("", "", "", "a", "a", ()),
        ("a", "a", "", "", "", ("(-a%*)",)),
        ("a", "a", "", "", "a", ("(-a%*)",)),
        ("", "a", "", "", "", ("(-a%)",)),
        # Flags outside IUSE, like the arch, never count.
        ("x86", "", "amd64", "", "", ()),
    ],
)
def test_rebuild_flags(old_use, old_iuse, use, iuse, forced, flags):
    sets = [
        frozenset(value.split()) for value in (old_use, old_iuse, use, iuse, forced)
    ]
    assert evaluated.rebuild_flags(*sets) == flags
