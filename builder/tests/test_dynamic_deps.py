"""Dynamic dependencies: the oracle against emerge's own FakeVartree, then egraph against both.

emerge re-reads an installed package's dependencies from its ebuild by default (--dynamic-deps=y).
The oracle reproduces FakeVartree's rule with portage's public API; these tests hold it to
FakeVartree itself (test code only, as depclean.py uses the resolver), and hold egraph to the
oracle and to depclean.
"""

import os
import subprocess

import pytest

from conftest import fake_vartree, portdb, write_stores
from depclean import depclean
from egraph_build import oracle
from egraph_build.model import DEP_KINDS, Edge

EGRAPH = os.environ.get("EGRAPH")


def test_oracle_reads_what_emerge_reads(scenario):
    fake = fake_vartree(scenario)
    for cpv in oracle.installed(scenario.vardb):
        _, strings, _ = oracle.dynamic_dep_strings(
            scenario.vardb, portdb(scenario), cpv
        )
        emerge = dict(zip(DEP_KINDS, fake.dbapi.aux_get(cpv, list(DEP_KINDS))))
        assert strings == emerge, cpv


@pytest.fixture
def stored(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    return scenario, path


def egraph(path, *args):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "--layout", "lines", *args],
        capture_output=True,
        text=True,
    )


@pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary")
def test_egraph_deps_follow_the_ebuilds(stored):
    system, path = stored
    for cpv in oracle.installed(system.vardb):
        result = egraph(path, "deps", "--dynamic-deps", "y", cpv)
        assert result.returncode == 0, result.stderr
        found = set()
        for line in result.stdout.splitlines():
            parent, kind, atom, child, *choice = line.split("\t")
            found.add(Edge(parent, child, kind, atom, choice == ["any-of"]))
        assert found == oracle.dynamic_deps(system.vardb, portdb(system), cpv), cpv


@pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary")
def test_egraph_orphans_follow_the_ebuilds(stored):
    system, path = stored
    expected = depclean(system.trees, system.eroot, dynamic_deps=True)
    result = egraph(path, "orphans", "--dynamic-deps", "y")
    assert (result.returncode != 0) == (expected.returncode != 0), result.stderr
    if expected.kept:
        assert tuple(result.stdout.splitlines()) == expected.orphans
