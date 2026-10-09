"""The build history: each build egraph exec runs recorded as it merges or fails, held to the
run's trace, log and vdb entries, and read back by egraph builds."""

import json
import os

import pytest

from egraph_build import worker
from test_actions import EGRAPH
from test_build import age
from test_exec_run import (
    logged,
    machines,
    parallel,
    read_trace,
)  # noqa: F401 (fixtures)

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def history(system):
    path = os.path.join(system.playground.eroot, "var", "lib", "egraph", "builds.jsonl")
    with open(path) as f:
        return [json.loads(line) for line in f]


def beside(trace):
    """By cpv, the most other builds running at once beside its build."""
    running = set()
    most = {}
    for what, cpv in trace:
        if what == "build-start":
            running.add(cpv)
            for each in running:
                most[each] = max(most.get(each, 0), len(running) - 1)
        elif what in ("built", "build-failed"):
            running.discard(cpv)
    return most


def built_with(machine, cpv):
    """The flags a merged package was built with, of its IUSE, as its vdb entry records them."""
    use, iuse = machine.playground.trees[machine.eroot]["vartree"].dbapi.aux_get(
        cpv, ["USE", "IUSE"]
    )
    return sorted(set(use.split()) & {flag.lstrip("+-") for flag in iuse.split()})


def test_each_build_is_recorded_as_it_merges(parallel, tmp_path):
    """In the order they merge, under the run's id, with every phase timed, its flags, its
    environment and the builds beside it."""
    system, machine = parallel
    age(system.playground.eroot)
    trace = tmp_path / "trace"
    ran = system.egraph("exec", "--yes", "-j3", "--trace", str(trace), "app-misc/top")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    traced = read_trace(trace)
    records = history(system)
    assert [r["cpv"] for r in records] == [c for w, c in traced if w == "merged"]
    (run,) = {e["run"] for e in logged(system)}
    most = beside(traced)
    for record in records:
        cpv = record["cpv"]
        assert record["version"] == 1
        assert (record["origin"], record["run"]) == ("exec", run)
        assert (record["repo"], record["slot"]) == ("test_repo", "0")
        assert record["outcome"] == "merged"
        assert [p["phase"] for p in record["phases"]] == list(worker.PHASES)
        assert sum(p["seconds"] for p in record["phases"]) <= record["build_seconds"]
        # Each compiles for a second.
        assert record["build_seconds"] >= 1
        assert record["merge_seconds"] > 0 and record["waited"] >= 0
        assert record["use"] == built_with(machine, cpv)
        assert set(record["environment"]) <= set(worker.ENVIRONMENT)
        assert record["jobs"] == 3
        assert record["beside"] == most[cpv], cpv


def test_a_failed_build_is_recorded_with_its_phase_and_log(machines):
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/broken")
    assert ran.returncode != 0
    (record,) = history(system)
    assert record["cpv"] == "app-misc/broken-1"
    assert record["outcome"] == "failed"
    assert record["failed_phase"] == "compile"
    assert record["status"] != 0
    assert os.path.isfile(record["log"])
    assert record["phases"][-1]["phase"] == "compile"
    assert "merge_seconds" not in record


def test_the_history_keeps_build_keep_builds_of_each_package(machines):
    system, machine = machines
    conf = os.path.join(system.playground.eroot, "etc", "egraph", "egraph.conf")
    os.makedirs(os.path.dirname(conf), exist_ok=True)
    with open(conf, "w") as f:
        f.write("build_keep = 1\n")
    for _ in range(2):
        age(system.playground.eroot)
        ran = system.egraph("exec", "--yes", "-1", "app-misc/lib")
        assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    assert [r["cpv"] for r in records] == ["app-misc/lib-1"]
    assert records[0]["run"] == logged(system)[-1]["run"]


def test_egraph_builds_reads_the_history_back(machines):
    """As lines, for the packages given, as the file holds them and as CSV."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/needs")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    assert len(records) >= 2
    listed = system.egraph("builds")
    assert listed.returncode == 0, listed.stderr
    rows = [line.split("\t") for line in listed.stdout.splitlines()]
    assert [(row[1], row[2], row[3]) for row in rows] == [
        (r["cpv"], r["repo"], r["outcome"]) for r in records
    ]
    one = system.egraph("builds", "app-misc/needs")
    assert [line.split("\t")[1] for line in one.stdout.splitlines()] == [
        "app-misc/needs-1"
    ]
    as_json = system.egraph("builds", "--format", "json")
    assert [json.loads(line) for line in as_json.stdout.splitlines()] == records
    as_csv = system.egraph("builds", "--format", "csv").stdout.splitlines()
    assert as_csv[0].startswith("start,origin,run,cpv,")
    assert len(as_csv) == len(records) + 1
    bad = system.egraph("builds", "not an atom")
    assert bad.returncode == 2
