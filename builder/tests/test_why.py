"""egraph why against the parents emerge --depclean records, through the real binary."""

import os
import subprocess

import portage
import pytest

from depclean import depclean
from egraph_build import installed, oracle, store

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def why(path, argument, *options):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "why", *options, argument],
        capture_output=True,
        text=True,
    )


def distances(parents, vardb):
    """Shortest number of dependencies from a root set to each kept cpv.

    depclean records a root set as a parent of every kept package its atom matches, but the
    atom only keeps the highest one; any other is kept by a dependency, and that is the answer.
    """
    children = {}
    frontier = []
    for child, pairs in parents.items():
        for parent, atom in pairs:
            if parent.startswith("@"):
                if portage.best(vardb.match(atom)) == child:
                    frontier.append(child)
            else:
                children.setdefault(parent, set()).add(child)
    found = {cpv: 0 for cpv in frontier}
    while frontier:
        following = []
        for cpv in frontier:
            for child in children.get(cpv, ()):
                if child not in found:
                    found[child] = found[cpv] + 1
                    following.append(child)
        frontier = following
    return found


def assert_explains(stdout, cpv, expected, vardb):
    """stdout is a shortest chain of depclean's own parent links from a root to cpv."""
    root, *edges = [line.split("\t") for line in stdout.splitlines()]
    root_set, root_atom, current = root
    assert root_set.startswith("@")
    assert any(
        parent.startswith("@") and atom == root_atom
        for parent, atom in expected.parents[current]
    )
    for parent, _kind, _atom, child, *_ in edges:
        assert parent == current
        assert parent in {p for p, _ in expected.parents[child]}
        current = child
    assert current == cpv
    assert len(edges) == distances(expected.parents, vardb)[cpv]


@pytest.fixture
def system(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(
        path,
        store.encode(installed.build(scenario.vardb), store.Meta("0", "0", "/", 0)),
    )
    return scenario, path


@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_why_is_a_shortest_chain_depclean_follows(system, with_bdeps):
    scenario, path = system
    expected = depclean(scenario.trees, scenario.eroot, with_bdeps)
    option = ("--with-bdeps", "y" if with_bdeps else "n")
    for cpv in oracle.installed(scenario.vardb):
        result = why(path, cpv, *option)
        if cpv in expected.kept:
            assert result.returncode == 0, result.stderr
            assert_explains(result.stdout, cpv, expected, scenario.vardb)
        else:
            assert result.returncode == 1
            assert result.stdout == ""
            assert result.stderr == (
                f"egraph: why: {cpv}: nothing keeps it; depclean would remove it\n"
            )


def test_why_explains_every_match(playgrounds, tmp_path):
    vardb = playgrounds("roots").vardb
    path = tmp_path / "installed.egraph"
    store.write(
        path, store.encode(installed.build(vardb), store.Meta("0", "0", "/", 0))
    )
    result = why(path, "sys-kernel/sources")
    assert result.returncode == 1
    assert result.stdout == "@selected\tsys-kernel/sources\tsys-kernel/sources-2\n"
    assert "sys-kernel/sources-1: nothing keeps it" in result.stderr

    result = why(path, "dev-libs/impl-b")
    assert result.stdout.splitlines() == [
        "@selected\tapp-misc/world\tapp-misc/world-1",
        "app-misc/world-1\tRDEPEND\tdev-libs/impl-b\tdev-libs/impl-b-1",
    ]
