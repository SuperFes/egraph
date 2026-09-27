"""egraph's queries against portage's answers, through the real binary."""

import json
import os
import subprocess

import pytest

from compare import _sonames
from egraph_build import installed, oracle, store
from egraph_build.model import Edge

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def egraph(path, *args, check=True):
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", *args],
        capture_output=True,
        text=True,
    )
    if check:
        assert result.returncode == 0, result.stderr
    return result


def parse_edges(text):
    edges = set()
    for line in text.splitlines():
        parent, kind, atom, child, *choice = line.split("\t")
        edges.add(Edge(parent, child, kind, atom, choice == ["any-of"]))
    return frozenset(edges)


@pytest.fixture
def system(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(
        path,
        store.encode(installed.build(scenario.vardb), store.Meta("0", "0", "/", 0)),
    )
    return scenario.vardb, path


def test_deps_and_rdeps(system):
    vardb, path = system
    for cpv in oracle.installed(vardb):
        assert parse_edges(egraph(path, "deps", cpv).stdout) == oracle.deps(vardb, cpv)
        assert parse_edges(egraph(path, "rdeps", cpv).stdout) == oracle.rdeps(
            vardb, cpv
        )


def test_a_cp_names_every_installed_version(playgrounds, tmp_path):
    vardb = playgrounds("reference").vardb
    path = tmp_path / "installed.egraph"
    store.write(
        path, store.encode(installed.build(vardb), store.Meta("0", "0", "/", 0))
    )
    expected = oracle.rdeps(vardb, "dev-libs/lib-1") | oracle.rdeps(
        vardb, "dev-libs/lib-2"
    )
    assert parse_edges(egraph(path, "rdeps", "dev-libs/lib").stdout) == expected
    both = egraph(path, "deps", "app-misc/root-1", "app-misc/user-1").stdout
    assert parse_edges(both) == oracle.deps(vardb, "app-misc/root-1") | oracle.deps(
        vardb, "app-misc/user-1"
    )


def test_unknown_package(system):
    _, path = system
    result = egraph(path, "rdeps", "app-misc/nonexistent", check=False)
    assert result.returncode == 1
    assert (
        result.stderr == "egraph: app-misc/nonexistent: no installed package matches\n"
    )


def test_sonames(system):
    vardb, path = system
    for soname in _sonames(vardb):
        for flag, query in (
            ([], oracle.soname_consumers),
            (["--providers"], oracle.soname_providers),
        ):
            lines = egraph(path, "soname", *flag, soname).stdout.splitlines()
            assert lines == sorted(
                f"{u.cpv}\t{u.multilib_category}" for u in query(vardb, soname)
            )


def test_broken(system):
    vardb, path = system
    expected = sorted("\t".join(item) for item in oracle.broken(vardb))
    assert egraph(path, "broken").stdout.splitlines() == expected


def test_stats(system):
    vardb, path = system
    lines = dict(
        line.split(": ", 1) for line in egraph(path, "stats").stdout.splitlines()
    )
    cpvs = oracle.installed(vardb)
    assert int(lines["packages"]) == len(cpvs)
    assert int(lines["edges"]) == len(
        frozenset().union(*(oracle.deps(vardb, c) for c in cpvs))
    )
    assert int(lines["unsatisfied"]) == len(oracle.broken(vardb))


def _neighborhood(vardb, roots, depth, forward, reverse):
    seen = set(roots)
    frontier = list(roots)
    for _ in range(depth):
        found = []
        for cpv in frontier:
            if reverse:
                found += [edge.parent for edge in oracle.rdeps(vardb, cpv)]
            if forward:
                found += [edge.child for edge in oracle.deps(vardb, cpv)]
        frontier = [cpv for cpv in dict.fromkeys(found) if cpv not in seen]
        seen.update(frontier)
    return seen


@pytest.mark.parametrize(
    "direction, forward, reverse",
    [("reverse", False, True), ("forward", True, False), ("both", True, True)],
)
@pytest.mark.parametrize("depth", [0, 1, 2, 10])
def test_export_neighborhoods(system, direction, forward, reverse, depth):
    vardb, path = system
    for cpv in oracle.installed(vardb):
        args = ["--depth", str(depth), "--direction", direction, cpv]
        exported = egraph(path, "export", "--format", "json", *args).stdout
        members = {pkg["cpv"] for pkg in json.loads(exported)["packages"]}
        assert members == _neighborhood(vardb, [cpv], depth, forward, reverse)

        dot = egraph(path, "export", *args).stdout
        arrows = {
            tuple(part.strip().strip('"') for part in line.split("[")[0].split("->"))
            for line in dot.splitlines()
            if "->" in line
        }
        induced = {
            (edge.parent, edge.child)
            for member in members
            for edge in oracle.deps(vardb, member)
            if edge.child in members
        }
        assert arrows == induced
