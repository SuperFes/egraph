"""egraph orphans against emerge --depclean, through the real binary."""

import os
import subprocess

import pytest

from depclean import depclean
from egraph_build import installed, store

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def orphans(path, *options):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "orphans", *options],
        capture_output=True,
        text=True,
    )


def unresolved(stderr):
    """(parent, atom) pairs from egraph's refusal message."""
    return frozenset(
        tuple(line.strip().split("\t")[::2])
        for line in stderr.splitlines()
        if line.startswith("  ")
    )


@pytest.fixture
def system(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(
        path,
        store.encode(installed.build(scenario.vardb), store.Meta("0", "0", "/", 0)),
    )
    return scenario, path


@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_orphans_are_what_depclean_removes(system, with_bdeps):
    scenario, path = system
    expected = depclean(scenario.trees, scenario.eroot, with_bdeps)
    result = orphans(path, "--with-bdeps", "y" if with_bdeps else "n")
    assert (result.returncode != 0) == (expected.returncode != 0), result.stderr
    if not expected.kept:
        # Refused before resolving anything: an empty @world.
        assert result.stdout == ""
        assert "the @world set is empty" in result.stderr
        return
    assert tuple(result.stdout.splitlines()) == expected.orphans
    assert unresolved(result.stderr) == expected.unresolved


def test_roots_scenario_by_hand(playgrounds, tmp_path):
    """What depclean removes there, worked out by hand."""
    vardb = playgrounds("roots").vardb
    path = tmp_path / "installed.egraph"
    store.write(
        path, store.encode(installed.build(vardb), store.Meta("0", "0", "/", 0))
    )
    result = orphans(path)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "app-misc/orphan-1",
        # || ( alt-x alt-y ): neither kept yet, so the first.
        "dev-libs/alt-y-1",
        # A cycle nothing else needs.
        "dev-libs/cycle-a-1",
        "dev-libs/cycle-b-1",
        # || ( impl-a impl-b ) in virtual/v: impl-b is already kept.
        "dev-libs/impl-a-1",
        "dev-libs/orphan-dep-1",
        # The unslotted world atom keeps the highest slot only.
        "sys-kernel/sources-1",
    ]


def test_dependencies_are_the_ones_recorded_at_merge(playgrounds, tmp_path):
    """The one known divergence: depclean's default --dynamic-deps=y reads dependencies from an
    installed package's ebuild when it still exists; egraph reads the vdb (docs/design.md).
    """
    system = playgrounds("dynamic-deps")
    path = tmp_path / "installed.egraph"
    store.write(
        path,
        store.encode(installed.build(system.vardb), store.Meta("0", "0", "/", 0)),
    )
    assert orphans(path).stdout == ""
    assert depclean(system.trees, system.eroot, dynamic_deps=False).orphans == ()
    assert depclean(system.trees, system.eroot, dynamic_deps=True).orphans == (
        "dev-libs/dep-1",
    )
