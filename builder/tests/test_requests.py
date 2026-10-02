"""The worker's requests for a plan, held to what emerge's scheduler would hand each step."""

import json
import os

import pytest

from conftest import write_stores
from egraph_build.cli import EXIT_REFUSED
from resume import resumed, same_version, worker_requests
from scenarios import SCENARIOS
from test_queries import PLAN_MODES, egraph, plan_requests
from test_resume import UPDATE_MODES

pytestmark = pytest.mark.skipif(
    not os.environ.get("EGRAPH"),
    reason="set EGRAPH to the egraph binary (meson test does)",
)


def check_requests(system, result, resume, requests):
    """There is a request for each merge and uninstall emerge's resume holds, and each one's
    blockers, world atom and world cleaning are emerge's for the step, the steps run in egraph's
    order; written exactly when the resume list is."""
    assert resume.exists() == requests.exists()
    if result.returncode == EXIT_REFUSED or not resume.exists():
        return
    entry = json.loads(resume.read_text())
    found = [json.loads(line) for line in requests.read_text().splitlines()]
    resume.unlink()
    requests.unlink()
    done = resumed(system.trees, system.eroot, entry)
    merges = [(r["cpv"], r["repo"]) for r in found if "cpv" in r]
    uninstalls = [r["uninstall"] for r in found if "uninstall" in r]
    assert len(merges) + len(uninstalls) == len(found)
    spell = same_version({cpv for cpv, _ in merges})
    assert sorted(merges) == sorted((spell(cpv), repo) for cpv, repo in done.merges)
    assert sorted(uninstalls) == sorted(done.uninstalls)
    for request in found:
        if "blockers" in request:
            request["blockers"] = sorted(request["blockers"])
    expected = worker_requests(system.trees, system.eroot, entry, found)
    assert expected is not None
    assert found == expected


@pytest.mark.parametrize("mode", sorted(UPDATE_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_update_requests_are_emerges(playgrounds, tmp_path, name, mode):
    system = playgrounds(name)
    store = tmp_path / "installed.egraph"
    write_stores(system, store, eroot=system.eroot)
    resume, requests = tmp_path / "resume.json", tmp_path / "requests"
    result = egraph(
        store,
        "updates",
        *UPDATE_MODES[mode],
        "--resume-list",
        str(resume),
        "--requests",
        str(requests),
    )
    check_requests(system, result, resume, requests)


@pytest.mark.parametrize("mode", sorted(PLAN_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_plan_requests_are_emerges(playgrounds, tmp_path, name, mode):
    system = playgrounds(name)
    store = tmp_path / "installed.egraph"
    write_stores(system, store, request_all=True, eroot=system.eroot)
    resume, requests = tmp_path / "resume.json", tmp_path / "requests"
    _, flags = PLAN_MODES[mode]
    for target in plan_requests(name):
        result = egraph(
            store,
            "plan",
            *flags,
            "--resume-list",
            str(resume),
            "--requests",
            str(requests),
            target,
            check=False,
        )
        if result.returncode == 1:
            continue
        assert result.returncode in (0, EXIT_REFUSED), (target, result.stderr)
        check_requests(system, result, resume, requests)
