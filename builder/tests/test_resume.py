"""--resume-list: egraph's plan as the merge list emerge resumes, held to emerge's resume of it."""

import json
import os

import pytest

from conftest import write_stores
from egraph_build.cli import EXIT_REFUSED
from resume import resumed
from scenarios import SCENARIOS
from test_queries import (
    PLAN_MODES,
    blocker_rows,
    egraph,
    merge_lines,
    plan_requests,
    ties,
)

pytestmark = pytest.mark.skipif(
    not os.environ.get("EGRAPH"),
    reason="set EGRAPH to the egraph binary (meson test does)",
)

UPDATE_MODES = {"u": [], "uDN": ["-D", "-N"], "world": ["--world", "-D"]}


def planned(text):
    """(cpv, repo) for each merge in updates or plan output."""
    return frozenset(
        (fields[2], fields[3])
        for fields in (line.split("\t") for line in merge_lines(text))
    )


def check_resumed(system, result, path):
    """Whether emerge resumes the list as egraph planned it: every merge kept, nothing added,
    and the same uninstalls for blockers."""
    if result.returncode == EXIT_REFUSED:
        assert not path.exists()
        return
    if not path.exists():
        # An empty set asks for nothing.
        assert result.stdout == "", result.args
        return
    entry = json.loads(path.read_text())
    path.unlink()
    merges = planned(result.stdout)
    assert [merge[2] for merge in entry["mergelist"]] != [] or not merges
    if not merges:
        return
    found = resumed(system.trees, system.eroot, entry)
    assert found.success
    assert not found.dropped
    # =cpv matches equal versions spelled otherwise, which emerge picks among in directory
    # order.
    assert ties(
        {("", cpv, repo) for cpv, repo in found.merges},
        {("", cpv, repo) for cpv, repo in merges},
    )
    assert found.uninstalls == blocker_rows(result.stdout)[0]


@pytest.mark.parametrize("mode", sorted(UPDATE_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_updates_resume_as_planned(playgrounds, tmp_path, name, mode):
    system = playgrounds(name)
    store = tmp_path / "installed.egraph"
    write_stores(system, store, eroot=system.eroot)
    path = tmp_path / "resume.json"
    result = egraph(store, "updates", *UPDATE_MODES[mode], "--resume-list", str(path))
    check_resumed(system, result, path)


@pytest.mark.parametrize("mode", sorted(PLAN_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_plans_resume_as_planned(playgrounds, tmp_path, name, mode):
    system = playgrounds(name)
    store = tmp_path / "installed.egraph"
    write_stores(system, store, request_all=True, eroot=system.eroot)
    path = tmp_path / "resume.json"
    _, flags = PLAN_MODES[mode]
    for target in plan_requests(name):
        result = egraph(
            store, "plan", *flags, "--resume-list", str(path), target, check=False
        )
        if result.returncode == 1:
            # Nothing matches the argument.
            continue
        assert result.returncode in (0, EXIT_REFUSED), (target, result.stderr)
        check_resumed(system, result, path)
