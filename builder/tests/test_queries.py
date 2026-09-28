"""egraph's queries against portage's answers, through the real binary."""

import json
import os
import subprocess

import pytest

from compare import _sonames, possible_mismatches
from conftest import dynamic_option, portdb, write_stores
from egraph_build import oracle, roots
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
    write_stores(scenario, path)
    return scenario.vardb, path


def test_deps_and_rdeps(scenario, system, dynamic_deps):
    vardb, path = system
    ebuilds = portdb(scenario) if dynamic_deps else None
    option = dynamic_option(dynamic_deps)
    for cpv in oracle.installed(vardb):
        found = parse_edges(egraph(path, "deps", *option, cpv).stdout)
        assert found == oracle.deps(vardb, cpv, portdb=ebuilds), cpv
        found = parse_edges(egraph(path, "rdeps", *option, cpv).stdout)
        assert found == oracle.rdeps(vardb, cpv, portdb=ebuilds), cpv


def parse_possible(text):
    """(edges, possible): deps or rdeps --possible output split into the edges installed
    packages have and the (Edge, flags) pairs toggled flags would add."""
    edges, possible = set(), set()
    for line in text.splitlines():
        parent, kind, atom, child, *markers = line.split("\t")
        edge = Edge(parent, child, kind, atom, "any-of" in markers)
        toggles = [m[len("use=") :] for m in markers if m.startswith("use=")]
        if toggles:
            possible.add((edge, tuple(toggles[0].split())))
        else:
            edges.add(edge)
    return frozenset(edges), frozenset(possible)


def test_possible_deps_and_rdeps(scenario, system):
    vardb, path = system
    ebuilds = portdb(scenario)
    every = set()
    for cpv in oracle.installed(vardb):
        edges, possible = parse_possible(egraph(path, "deps", "--possible", cpv).stdout)
        deps = oracle.deps(vardb, cpv, portdb=ebuilds)
        assert edges == deps, cpv
        assert possible_mismatches(vardb, ebuilds, cpv, possible, deps) == [], cpv
        every |= possible
    for cpv in oracle.installed(vardb):
        edges, possible = parse_possible(
            egraph(path, "rdeps", "--possible", cpv).stdout
        )
        assert edges == oracle.rdeps(vardb, cpv, portdb=ebuilds), cpv
        assert possible == {(edge, flags) for edge, flags in every if edge.child == cpv}


def test_possible_dependencies_name_their_flags(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("possible"), path)
    assert egraph(path, "rdeps", "--possible", "dev-libs/deep").stdout == (
        "app-misc/host-1\tRDEPEND\tdev-libs/deep\tdev-libs/deep-1\tuse=a b c\n"
    )
    assert egraph(path, "rdeps", "--possible", "dev-libs/w2").stdout == (
        "app-misc/host-1\tRDEPEND\tdev-libs/w2\tdev-libs/w2-1\tany-of\tuse=a\n"
    )
    assert egraph(path, "rdeps", "--possible", "dev-libs/z").stdout == (
        "app-misc/host-1\tRDEPEND\tdev-libs/z\tdev-libs/z-1\tuse=-minimal\n"
    )
    # Masked and forced flags stay as they are, and arch flags are the profile's.
    for name in ("m", "f", "arch", "never"):
        assert egraph(path, "rdeps", "--possible", f"dev-libs/{name}").stdout == ""
    result = egraph(
        path, "rdeps", "--possible", "--dynamic-deps", "n", "dev-libs/z", check=False
    )
    assert result.returncode == 2


def test_a_cp_names_every_installed_version(playgrounds, tmp_path):
    reference = playgrounds("reference")
    vardb = reference.vardb
    path = tmp_path / "installed.egraph"
    write_stores(reference, path)
    expected = oracle.rdeps(vardb, "dev-libs/lib-1") | oracle.rdeps(
        vardb, "dev-libs/lib-2"
    )
    rdeps = egraph(path, "rdeps", "--dynamic-deps", "n", "dev-libs/lib").stdout
    assert parse_edges(rdeps) == expected
    both = egraph(
        path, "deps", "--dynamic-deps", "n", "app-misc/root-1", "app-misc/user-1"
    ).stdout
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


def test_broken(scenario, system, dynamic_deps):
    vardb, path = system
    ebuilds = portdb(scenario) if dynamic_deps else None
    expected = sorted("\t".join(item) for item in oracle.broken(vardb, ebuilds))
    found = egraph(path, "broken", *dynamic_option(dynamic_deps)).stdout
    assert found.splitlines() == expected


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
    atoms = roots.root_atoms(vardb)
    assert int(lines["root atoms"]) == sum(len(atoms[name]) for name in roots.ROOT_SETS)


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
