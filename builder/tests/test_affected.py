"""egraph affected against the portage fork's own index, on every scenario.

The fork's neighborhood completion asks its InstalledGraph what can be reached from the required
sets, what a merge's blockers match, and which installed packages a set of merges can affect.
egraph affected answers the same in one call; this holds it to the fork's answers.
"""

import json
import os
import subprocess

import pytest

from egraph_build import installed, store

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary")

index_module = pytest.importorskip(
    "portage.dbapi._InstalledGraph", reason="needs the portage fork's InstalledGraph"
)
neighborhood = pytest.importorskip(
    "_emerge.resolver.neighborhood",
    reason="needs the portage fork's neighborhood completion",
)

KIND_SETS = [
    ("RDEPEND", "IDEPEND", "PDEPEND"),
    ("RDEPEND", "IDEPEND", "PDEPEND", "DEPEND", "BDEPEND"),
    ("RDEPEND", "IDEPEND", "PDEPEND", "DEPEND", "BDEPEND", "SONAME"),
    ("SONAME",),
]


@pytest.fixture
def system(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(
        path,
        store.encode(installed.build(scenario.vardb), store.Meta("0", "0", "/", 0)),
    )
    index = index_module.InstalledGraph.from_vardb(scenario.vardb)

    def ask(**request):
        result = subprocess.run(
            [EGRAPH, "--store", str(path), "--no-refresh", "affected"],
            input=json.dumps(request),
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)

    return index, ask


def atoms(index):
    """Every atom in every installed package's dependencies, blockers included."""
    return sorted({str(edge.atom) for pkg in index for edge in pkg.deps})


def test_reachable_matches_the_fork(system):
    index, ask = system
    cpvs = sorted(pkg.cpv for pkg in index)
    for kinds in KIND_SETS:
        for seeds in [[cpv] for cpv in cpvs] + [cpvs]:
            answer = ask(kinds=list(kinds), seeds=seeds)
            assert answer["reachable"] == sorted(index.reachable(seeds, set(kinds))), (
                kinds,
                seeds,
            )


def test_blockers_match_as_the_fork_matches(system):
    index, ask = system
    blockers = [atom for atom in atoms(index) if atom.startswith("!")]
    for atom in blockers:
        answer = ask(blockers=[atom])
        assert answer["blocked"] == sorted(index.matches(index_module.Atom(atom))), atom


def test_affected_matches_the_fork(system):
    index, ask = system
    cps = sorted(
        {pkg.cp for pkg in index} | {edge.atom.cp for pkg in index for edge in pkg.deps}
    )
    cpvs = sorted(pkg.cpv for pkg in index)
    blockers = [atom for atom in atoms(index) if atom.startswith("!")]

    def expected(changed=(), replaced=(), blocked_atoms=()):
        blocked = [
            cpv
            for atom in blocked_atoms
            for cpv in index.matches(index_module.Atom(atom))
        ]
        return sorted(neighborhood.affected_cpvs(index, changed, replaced, blocked))

    for cp in cps:
        assert ask(changed=[cp])["affected"] == expected(changed=[cp]), cp
    for cpv in cpvs:
        assert ask(replaced=[cpv])["affected"] == expected(replaced=[cpv]), cpv
    for atom in blockers:
        assert ask(blockers=[atom])["affected"] == expected(blocked_atoms=[atom]), atom
    everything = ask(changed=cps, replaced=cpvs, blockers=blockers)
    assert everything["affected"] == expected(cps, cpvs, blockers)
