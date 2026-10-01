"""egraph remove against emerge --depclean with arguments, through the real binary."""

import os
import subprocess

import pytest

from conftest import dynamic_option, write_stores
from depclean import depclean

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def removed(path, argument, *options):
    """What egraph remove lists for removal; it stops before emerge, with no terminal."""
    result = subprocess.run(
        [
            EGRAPH,
            "--store",
            str(path),
            "--no-refresh",
            "--layout",
            "lines",
            "remove",
            *options,
            argument,
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode in (0, 1, 2), result.stderr
    return tuple(
        line.split("\t")[0]
        for line in result.stdout.splitlines()
        if line.split("\t")[1] == "remove"
    )


def arguments(scenario):
    """Each installed cp, and =cpv for each version of a cp with several installed."""
    vardb = scenario.vardb
    found = []
    for cp in sorted(vardb.cp_all()):
        found.append(cp)
        cpvs = vardb.cp_list(cp)
        if len(cpvs) > 1:
            found.extend(f"={cpv}" for cpv in sorted(cpvs))
    return found


@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_removals_are_what_depclean_removes_of_its_arguments(
    scenario, tmp_path, with_bdeps, dynamic_deps
):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    differences = []
    for argument in arguments(scenario):
        expected = depclean(
            scenario.trees, scenario.eroot, with_bdeps, dynamic_deps, args=[argument]
        )
        if not expected.kept:
            # An empty @world: depclean refuses, and so does egraph.
            continue
        ours = removed(
            path,
            argument,
            "--with-bdeps",
            "y" if with_bdeps else "n",
            *dynamic_option(dynamic_deps),
        )
        if ours != expected.orphans:
            differences.append((argument, ours, expected.orphans))
    assert differences == []
