"""egraph remove against emerge --depclean, with arguments and without, through the real binary."""

import os
import subprocess

import pytest

from conftest import dynamic_option, write_stores
from depclean import depclean

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def remove(path, *arguments):
    """egraph remove; it stops before emerge, with no terminal."""
    return subprocess.run(
        [
            EGRAPH,
            "--store",
            str(path),
            "--no-refresh",
            "--layout",
            "lines",
            "remove",
            *arguments,
        ],
        capture_output=True,
        text=True,
    )


def removals(stdout):
    """The cpvs egraph remove lists for removal."""
    return tuple(
        line.split("\t")[0]
        for line in stdout.splitlines()
        if line.split("\t")[1] == "remove"
    )


def removed(path, argument, *options):
    """What egraph remove lists for removal of argument."""
    result = remove(path, *options, argument)
    assert result.returncode in (0, 1, 2), result.stderr
    return removals(result.stdout)


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


@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_removals_without_arguments_are_what_depclean_removes(
    scenario, tmp_path, with_bdeps, dynamic_deps
):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    expected = depclean(scenario.trees, scenario.eroot, with_bdeps, dynamic_deps)
    result = remove(
        path, "--with-bdeps", "y" if with_bdeps else "n", *dynamic_option(dynamic_deps)
    )
    if expected.returncode != 0:
        # An empty @world or unresolved runtime dependencies: refused, nothing listed.
        assert result.returncode == 1, result.stderr
        assert result.stdout == ""
        assert "refuse" in result.stderr or "is empty" in result.stderr
        return
    assert result.returncode in (0, 1, 2), result.stderr
    assert "refuse" not in result.stderr
    assert removals(result.stdout) == expected.orphans
