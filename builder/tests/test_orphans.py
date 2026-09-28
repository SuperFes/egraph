"""egraph orphans against emerge --depclean, through the real binary."""

import os
import subprocess

import pytest

from conftest import dynamic_option, write_stores
from depclean import depclean

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
    write_stores(scenario, path)
    return scenario, path


@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_orphans_are_what_depclean_removes(system, with_bdeps, dynamic_deps):
    scenario, path = system
    expected = depclean(scenario.trees, scenario.eroot, with_bdeps, dynamic_deps)
    result = orphans(
        path, "--with-bdeps", "y" if with_bdeps else "n", *dynamic_option(dynamic_deps)
    )
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
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("roots"), path)
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


def test_dependencies_follow_the_ebuild_unless_told_otherwise(playgrounds, tmp_path):
    """The ebuild dropped a dependency without a revision bump: emerge's default
    --dynamic-deps=y lets it go, and --dynamic-deps=n keeps what the vdb recorded."""
    system = playgrounds("dynamic-deps")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    assert orphans(path).stdout == "dev-libs/dep-1\n"
    assert depclean(system.trees, system.eroot, dynamic_deps=True).orphans == (
        "dev-libs/dep-1",
    )
    assert orphans(path, "--dynamic-deps", "n").stdout == ""
    assert depclean(system.trees, system.eroot, dynamic_deps=False).orphans == ()


def test_atoms_of_one_cp_share_what_they_keep(playgrounds, tmp_path):
    """slotop-1's ebuild says dev-libs/lib:=, which alone would keep lib-2, and emerge appends
    the dev-libs/lib:1/1= it was built with; emerge's _minimize_children then lets both atoms
    keep lib-1 and nothing keeps lib-2."""
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("repository"), path)
    assert "dev-libs/lib-2" in orphans(path).stdout.splitlines()
