"""The evaluated layer against the oracle on every scenario, and by hand on the repository one."""

import collections

import pytest
from portage import best
from portage.exception import InvalidAtom, InvalidDependString
from portage.versions import cpv_getkey

from egraph_build import evaluated, oracle
from egraph_build.model import DEP_KINDS

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
