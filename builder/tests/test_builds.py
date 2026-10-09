"""The build history: each build egraph exec runs recorded as it merges or fails, held to the
run's trace, log and vdb entries, and read back by egraph builds."""

import json
import os
import queue
import signal
import subprocess
import threading
import time

import pytest

from egraph_build import worker
from test_actions import EGRAPH
from test_build import age
from test_exec_run import (
    RUNS,
    logged,
    machines,
    over,
    parallel,
    published,
    read_trace,
)  # noqa: F401 (fixtures)
from test_worker import INSTALL, PLAIN

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
        assert (
            record["environment"]["PORTAGE_TMPDIR"]
            == system.playground.settings["PORTAGE_TMPDIR"]
        )
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


def test_a_build_records_why_the_plan_merged_it(machines):
    """New the first time; asked for again, a reinstall of the version it replaces."""
    system, machine = machines
    for _ in range(2):
        age(system.playground.eroot)
        ran = system.egraph("exec", "--yes", "-1", "app-misc/lib")
        assert ran.returncode == 0, ran.stdout + ran.stderr
    first, again = history(system)
    assert first["reason"] == "new" and "replaced" not in first
    assert again["reason"] == "reinstall"
    assert again["replaced"] == "app-misc/lib-1"
    listed = system.egraph("builds").stdout.splitlines()
    assert [line.split("\t")[-1] for line in listed] == ["new", "reinstall"]


def test_egraph_builds_says_what_rebuilds_fails_and_got_slower(machines):
    system, machine = machines
    for target in ("app-misc/lib", "app-misc/lib", "app-misc/broken"):
        age(system.playground.eroot)
        system.egraph("exec", "--yes", "-1", target)
    rebuilds = system.egraph("builds", "--rebuilds").stdout.splitlines()
    assert rebuilds == ["app-misc/lib\t2\t1\t0\t0\t0\t0\t1\t0\t\t"]
    (failing,) = [
        line.split("\t")
        for line in system.egraph("builds", "--failing").stdout.splitlines()
    ]
    assert failing[:5] == ["app-misc/broken", "1", "1", "0", "failing"]
    assert failing[6:8] == ["app-misc/broken-1", "compile:1"]
    assert os.path.isfile(failing[8])
    slower = system.egraph("builds", "--slower")
    assert (slower.returncode, slower.stdout) == (0, "")
    for view in ("--rebuilds", "--failing", "--slower"):
        shown = system.egraph("--layout", "human", "builds", view)
        assert shown.returncode == 0, shown.stderr
    assert system.egraph("builds", "--failing", "--slower").returncode == 2


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


def test_egraph_builds_views_sum_the_history_up(machines):
    """The slowest packages and the time by week, as lines and for people."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/needs")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    slowest = [
        line.split("\t")
        for line in system.egraph("builds", "--slowest").stdout.splitlines()
    ]
    assert sorted(row[0] for row in slowest) == sorted(
        r["cpv"].rsplit("-", 1)[0] for r in records
    )
    medians = [float(row[3]) for row in slowest]
    assert medians == sorted(medians, reverse=True)
    (week,) = [
        line.split("\t")
        for line in system.egraph("builds", "--by", "week").stdout.splitlines()
    ]
    assert int(week[1]) == len(records)
    spent = sum(r["build_seconds"] + r.get("merge_seconds", 0) for r in records)
    assert float(week[3]) == pytest.approx(spent, abs=0.1 * len(records))
    for view in (["--slowest"], ["--by", "month"], ["app-misc/needs"], []):
        shown = system.egraph("--layout", "human", "builds", *view)
        assert shown.returncode == 0, shown.stderr
        assert "Nothing in the build history" not in shown.stdout
    assert system.egraph("builds", "--slowest", "--format", "csv").returncode == 2
    assert system.egraph("builds", "--slowest", "--by", "week").returncode == 2


def estimate(printed):
    """A plan's estimate line: seconds, peak, known, unknown, jobs and guessed."""
    (line,) = [l for l in printed.splitlines() if l.startswith("\testimate\t")]
    return line.split("\t")[2:]


def advice(printed):
    """A plan's advice lines, each without its leading fields."""
    return [
        l.split("\t")[2:] for l in printed.splitlines() if l.startswith("\tadvice\t")
    ]


def test_a_plan_says_what_its_tests_cost(machines):
    """Builds whose test phases took long are named after the estimate, and the plan says when
    it would finish."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/needs")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    path = os.path.join(system.playground.eroot, "var", "lib", "egraph", "builds.jsonl")
    with open(path, "w") as f:
        for i, record in enumerate(records):
            tests = 100 * (i + 1)
            # Each build ran a short test phase of its own: it now takes tests.
            (phase,) = [p for p in record["phases"] if p["phase"] == "test"]
            record["build_seconds"] += tests - phase["seconds"]
            phase["seconds"] = tests
            f.write(json.dumps(record) + "\n")
    targets = ["=" + r["cpv"] for r in records]
    plan = system.egraph("plan", *targets)
    assert plan.returncode == 0, plan.stdout + plan.stderr
    total, *tested = advice(plan.stdout)
    costs = sorted(
        ((100 * (i + 1), r["cpv"]) for i, r in enumerate(records)), reverse=True
    )
    assert total == ["tests", str(sum(c for c, _ in costs))]
    assert tested == [["test", cpv, str(c)] for c, cpv in costs[:3]]
    human = system.egraph("--layout", "human", "plan", *targets)
    assert ", done about " in human.stdout
    assert "Tests: about " in human.stdout


def test_a_plan_says_when_its_builds_would_hold_most_of_the_memory(machines):
    """Builds that held nine tenths of this system's memory each fit at no jobs: the largest is
    named."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/needs")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    with open("/proc/meminfo") as f:
        (total,) = [
            int(line.split()[1]) * 1024 for line in f if line.startswith("MemTotal:")
        ]
    records = history(system)
    path = os.path.join(system.playground.eroot, "var", "lib", "egraph", "builds.jsonl")
    with open(path, "w") as f:
        for i, record in enumerate(records):
            record["peak_memory"] = total * 9 // 10 - i
            f.write(json.dumps(record) + "\n")
    targets = ["=" + r["cpv"] for r in records]
    plan = system.egraph("plan", *targets)
    assert plan.returncode == 0, plan.stdout + plan.stderr
    (memory,) = [a for a in advice(plan.stdout) if a[0] == "memory"]
    peak, size, jobs, seconds, jobs_peak, largest, largest_peak, floor = memory[1:]
    assert int(size) == total and int(peak) >= total * 9 // 10
    assert (jobs, seconds, jobs_peak) == ("", "", "")
    assert (largest, int(largest_peak)) == (records[0]["cpv"], total * 9 // 10)
    human = system.egraph("--layout", "human", "plan", *targets)
    assert f"{records[0]['cpv']} alone holds" in human.stdout


@pytest.mark.skipif(
    not any(
        line.split()[1:3] == ["/dev/shm", "tmpfs"] for line in open("/proc/mounts")
    ),
    reason="no tmpfs at /dev/shm",
)
def test_a_plan_offers_the_disk_to_builds_too_large_for_their_tmpfs(machines):
    """Builds whose directories grew past the tmpfs they built on are named, with the env that
    builds them on disk."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/needs")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    shm = os.statvfs("/dev/shm")
    size = shm.f_frsize * shm.f_blocks
    records = history(system)
    path = os.path.join(system.playground.eroot, "var", "lib", "egraph", "builds.jsonl")
    with open(path, "w") as f:
        for record in records:
            record["environment"]["PORTAGE_TMPDIR"] = "/dev/shm"
            record["build_dir_size"] = 2 * size
            f.write(json.dumps(record) + "\n")
    targets = ["=" + r["cpv"] for r in records]
    plan = system.egraph("plan", *targets)
    assert plan.returncode == 0, plan.stdout + plan.stderr
    tmpfs = [a[1:] for a in advice(plan.stdout) if a[0] == "tmpfs"]
    assert sorted(tmpfs) == sorted(
        [r["cpv"], str(2 * size), "/dev/shm", str(size)] for r in records
    )
    human = system.egraph("--layout", "human", "plan", *targets)
    assert "Tmpfs: " in human.stdout and "notmpfs.conf" in human.stdout


BINARY_EBUILDS = {
    "app-misc/same-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": INSTALL},
    "app-misc/flagged-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": INSTALL},
    "app-misc/grown-1": {**PLAIN, "IUSE": "doc extra", "MISC_CONTENT": INSTALL},
    "app-misc/moved-1": {
        **PLAIN,
        "IUSE": "doc",
        "RDEPEND": "app-misc/same",
        "MISC_CONTENT": INSTALL,
    },
    "app-misc/remote-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": INSTALL},
}
BINARIES = {
    # Built as the ebuild would be now.
    "app-misc/same-1": {"EAPI": "8", "IUSE": "doc", "USE": ""},
    # With a flag the configuration leaves off.
    "app-misc/flagged-1": {"EAPI": "8", "IUSE": "doc", "USE": "doc"},
    # Before the ebuild's IUSE grew.
    "app-misc/grown-1": {"EAPI": "8", "IUSE": "doc", "USE": ""},
    # Before the ebuild gained a dependency.
    "app-misc/moved-1": {"EAPI": "8", "IUSE": "doc", "USE": ""},
}


@pytest.fixture
def binaried(gnupg_home, tmp_path):
    """The binary ebuilds, with binary packages in PKGDIR and on a binhost."""
    remote = {"app-misc/remote-1": {"EAPI": "8", "IUSE": "doc", "USE": ""}}
    yield from over(
        BINARY_EBUILDS, tmp_path, binpkgs=BINARIES, binrepos={"remote": remote}
    )


def test_a_plan_names_the_long_builds_emerge_would_take_binaries_for(binaried):
    """The builds a plan names a binary package for are those emerge --getbinpkg takes one for:
    of the matching USE, IUSE and deps, from PKGDIR or a binhost portage fetched the index of.
    """
    system, machine = binaried
    targets = ["=" + cpv for cpv in sorted(BINARY_EBUILDS)]
    ran = system.egraph("exec", "--yes", *targets)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    path = os.path.join(system.playground.eroot, "var", "lib", "egraph", "builds.jsonl")
    with open(path, "w") as f:
        for record in records:
            record["build_seconds"] = 1000
            f.write(json.dumps(record) + "\n")
    # emerge fetches the binhost's index, and keeps its copy, as it plans.
    pretended = machine.emerge("--pretend --getbinpkg " + " ".join(targets))
    taken = sorted(
        line.split("]")[1].split()[0].split("::")[0]
        for line in pretended.stdout.splitlines()
        if line.startswith("[binary")
    )
    assert taken == ["app-misc/remote-1", "app-misc/same-1"], pretended.stdout
    age(system.playground.eroot)
    refreshed = system.egraph("refresh")
    assert refreshed.returncode == 0, refreshed.stdout + refreshed.stderr
    plan = system.egraph("plan", *targets)
    assert plan.returncode == 0, plan.stdout + plan.stderr
    binaries = [a[1:] for a in advice(plan.stdout) if a[0] == "binary"]
    assert sorted(b[0] for b in binaries) == taken
    kinds = {b[0]: (b[1], b[2]) for b in binaries}
    assert kinds["app-misc/same-1"][1] == "local"
    assert kinds["app-misc/remote-1"] == ("1000", "binhost")
    human = system.egraph("--layout", "human", "plan", *targets)
    assert "Binary: app-misc/same-1 is in " in human.stdout


def test_a_plan_says_how_long_it_will_take_from_the_history(parallel, tmp_path):
    """Its packages' builds run ahead at the jobs of the latest build: no shorter than the
    longest build, no longer than all of them one after another."""
    system, machine = parallel
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "-j3", "app-misc/top")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    targets = ["=" + r["cpv"] for r in records]
    plan = system.egraph("plan", *targets)
    assert plan.returncode == 0, plan.stdout + plan.stderr
    seconds, peak, known, unknown, jobs, guessed = estimate(plan.stdout)
    assert (int(known), int(unknown), jobs, guessed) == (len(records), 0, "3", "0")
    longest = max(r["build_seconds"] for r in records)
    together = sum(r["build_seconds"] + r["merge_seconds"] for r in records)
    assert longest - 1 <= int(seconds) <= together + 1
    if all("peak_memory" in r for r in records):
        assert int(peak) >= max(r["peak_memory"] for r in records)
    human = system.egraph("--layout", "human", "plan", *targets)
    assert "Estimate: about " in human.stdout and " at -j3" in human.stdout
    # exec at the jobs it is given; a package never built here counted apart, guessed from
    # those built, which inherit no eclass either.
    shown = subprocess.run(
        system.command("exec", "-j1", *targets, "app-misc/marked"),
        input="n\n",
        capture_output=True,
        text=True,
    )
    seconds, peak, known, unknown, jobs, guessed = estimate(
        shown.stdout.split("egraph:")[0]
    )
    assert (int(known), int(unknown), jobs, guessed) == (len(records), 0, "1", "1")
    assert int(seconds) >= together - 1


def test_exec_starts_the_builds_its_order_puts_first(parallel, tmp_path):
    """With a long build last in emerge's order, a plan is told of the order that finishes
    sooner, and exec --order starts that build first, as the history foretells it."""
    system, machine = parallel
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "-j2", "app-misc/top")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    path = os.path.join(system.playground.eroot, "var", "lib", "egraph", "builds.jsonl")
    with open(path, "w") as f:
        # also and base first, as emerge orders them, leave side to build after them.
        lengths = {
            "app-misc/also-1": 400,
            "app-misc/base-1": 400,
            "app-misc/side-1": 2000,
        }
        for record in records:
            record["build_seconds"] = lengths.get(record["cpv"], 10)
            record["jobs"] = 2
            f.write(json.dumps(record) + "\n")
    targets = ["=" + r["cpv"] for r in records]
    plan = system.egraph("plan", *targets)
    assert plan.returncode == 0, plan.stdout + plan.stderr
    (order,) = [a[1:] for a in advice(plan.stdout) if a[0] == "order"]
    name, seconds, emerge_seconds = order
    assert name in ("critical", "longest")
    assert int(seconds) <= int(emerge_seconds) - 300
    human = system.egraph("--layout", "human", "plan", *targets)
    assert f"Order: egraph exec --order={name}" in human.stdout
    traced = tmp_path / "trace"
    run = system.egraph(
        "exec", "--yes", "-j2", "--order=longest", "--trace", str(traced), *targets
    )
    assert run.returncode == 0, run.stdout + run.stderr
    trace = read_trace(traced)
    started = [cpv for what, cpv in trace if what == "build-start"]
    assert started[0] == "app-misc/side-1"
    assert sorted(started) == sorted(r["cpv"] for r in records)
    # Its own order planned, a run says nothing of another.
    shown = subprocess.run(
        system.command("exec", "--order=longest", *targets),
        input="n\n",
        capture_output=True,
        text=True,
    )
    assert not [a for a in advice(shown.stdout) if a[0] == "order"]


# built and fresh inherit the same eclass, and other none.
SIMILAR_ECLASSES = {"slow": ['S="${WORKDIR}"', "src_compile() { sleep 2; }"]}
SIMILAR = {
    "app-misc/built-1": {**PLAIN, "MISC_CONTENT": "inherit slow\n"},
    "app-misc/fresh-1": {**PLAIN, "MISC_CONTENT": "inherit slow\n"},
    "app-misc/other-1": {**PLAIN, "MISC_CONTENT": INSTALL},
}


@pytest.fixture
def similar(gnupg_home, tmp_path):
    yield from over(SIMILAR, tmp_path, eclasses=SIMILAR_ECLASSES)


def test_a_package_never_built_here_is_guessed_from_similar_ones(similar):
    """From the repository index's eclasses: fresh as long as built took, other unknown."""
    system, machine = similar
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/built")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    (record,) = history(system)
    indexed = system.egraph("search", "fresh")
    assert indexed.returncode == 0, indexed.stdout + indexed.stderr
    plan = system.egraph("plan", "app-misc/fresh", "app-misc/other")
    assert plan.returncode == 0, plan.stdout + plan.stderr
    seconds, peak, known, unknown, jobs, guessed = estimate(plan.stdout)
    assert (known, unknown, guessed) == ("0", "1", "1")
    assert abs(int(seconds) - record["build_seconds"] - record["merge_seconds"]) <= 1
    human = system.egraph(
        "--layout", "human", "plan", "app-misc/fresh", "app-misc/other"
    )
    assert "; 1 guessed from similar packages, 1 never built here" in human.stdout


def test_exec_publishes_the_time_it_has_left(parallel):
    """Its status file counts down from the plan's estimate as the run goes."""
    system, machine = parallel
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "-j3", "app-misc/top")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    records = history(system)
    together = sum(r["build_seconds"] + r["merge_seconds"] for r in records)
    age(system.playground.eroot)
    run = subprocess.Popen(
        system.command("exec", "--yes", "-j3", *("=" + r["cpv"] for r in records)),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    left = []
    try:
        while run.poll() is None:
            for _, snapshot in published(system.playground.eprefix, RUNS):
                if "egraph_remaining" in snapshot:
                    left.append(snapshot["egraph_remaining"])
            time.sleep(0.1)
    finally:
        if run.poll() is None:
            run.kill()
        run.wait()
    assert run.returncode == 0
    assert left, "no snapshot said how long the run had left"
    assert all(0 <= each <= together + 1 for each in left)
    assert left[-1] <= left[0]


# The compile spins for a while, then holds 100 MB in a shell variable across a sleep.
HUNGRY = INSTALL + """src_compile() {
	local i held
	for ((i = 0; i < 400000; i++)); do :; done
	held=$(head -c 100000000 /dev/zero | tr '\\0' x)
	sleep 2.5
	: "${#held}"
}
"""


@pytest.fixture
def hungry(gnupg_home, tmp_path):
    yield from over({"app-misc/hungry-1": {**PLAIN, "MISC_CONTENT": HUNGRY}}, tmp_path)


def test_a_build_is_measured_by_what_its_processes_used(hungry):
    """Its CPU time by phase, the memory its processes held, its IO and the system's load."""
    system, machine = hungry
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/hungry")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    (record,) = history(system)
    phases = {p["phase"]: p for p in record["phases"]}
    assert all("cpu_seconds" in p for p in record["phases"])
    # Beyond what an empty phase costs portage.
    assert phases["compile"]["cpu_seconds"] >= phases["configure"]["cpu_seconds"] + 0.2
    assert (
        sum(p["cpu_seconds"] for p in record["phases"]) <= record["cpu_seconds"] + 0.01
    )
    assert record["peak_memory"] >= 100_000_000
    assert record["measured"] in ("proc", "cgroup")
    # Its 100 MB were held in memory, not in its build directory.
    assert record["build_dir_size"] > record["image_size"] > 0
    assert record["distfiles_size"] == 0
    vdb = os.path.join(
        system.playground.eroot, "var", "db", "pkg", "app-misc", "hungry-1"
    )
    with open(os.path.join(vdb, "SIZE")) as f:
        assert record["installed_size"] == int(f.read())
    assert "fetch" in phases
    if os.path.exists("/proc/self/io"):
        assert record["read_bytes"] >= 0 and record["write_bytes"] >= 0
    shares = record["system"]
    assert 0 <= shares["cpu_busy"] <= 1
    assert shares["load"] >= 0
    if os.path.exists("/proc/pressure/cpu"):
        for key in ("stall_cpu", "stall_memory", "stall_io"):
            assert 0 <= shares[key] <= 1, key
    listed = system.egraph("builds", "--format", "csv").stdout.splitlines()
    header, row = listed[0].split(","), listed[1].split(",")
    assert int(row[header.index("peak_memory")]) == record["peak_memory"]


def cgroups_writable():
    from portage.const import SUPPORTED_FEATURES

    return (
        os.geteuid() == 0
        and os.path.exists("/sys/fs/cgroup/cgroup.controllers")
        and os.access("/sys/fs/cgroup", os.W_OK)
        and "cgroup" in SUPPORTED_FEATURES
    )


@pytest.mark.skipif(not cgroups_writable(), reason="needs root on a writable cgroup v2")
def test_a_build_as_root_is_measured_in_its_own_cgroup(hungry):
    """Its phases in a leaf of egraph's hierarchy, read exactly and removed with it."""
    system, machine = hungry
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/hungry")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    (record,) = history(system)
    assert record["measured"] == "cgroup"
    phases = {p["phase"]: p for p in record["phases"]}
    assert phases["compile"]["cpu_seconds"] >= phases["configure"]["cpu_seconds"] + 0.2
    assert record["peak_memory"] >= 100_000_000
    assert (
        abs(sum(p["cpu_seconds"] for p in record["phases"]) - record["cpu_seconds"])
        < 0.01
    )
    base = "/sys/fs/cgroup/egraph"
    assert not [n for n in os.listdir(base) if n.startswith("exec-")] or all(
        os.path.exists(f"/proc/{n[5:]}")
        for n in os.listdir(base)
        if n.startswith("exec-")
    )


# Its compile long enough for emerge's snapshots, which may each come 2 s late, to show it.
SLOW = INSTALL + "src_compile() { sleep 6; }\n"


@pytest.fixture
def observed_emerge(gnupg_home, tmp_path):
    config = {"make.conf": ('FEATURES="${FEATURES} observability"',)}
    yield from over(
        {"app-misc/slow-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": SLOW}},
        tmp_path,
        user_config=config,
    )


def test_egraphd_records_emerges_builds_as_emerges(observed_emerge):
    """Followed through emerge's snapshots, settled by the vdb entry, under emerge's pid."""
    system, machine = observed_emerge
    # Made by an earlier emerge, as /run/portage is on a system that ran one since it booted.
    os.makedirs(os.path.join(system.playground.eprefix, "run", "portage"))
    age(system.playground.eroot)
    watch = subprocess.Popen(
        system.command("watch"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        env=dict(os.environ, EGRAPH_LOG="file"),
    )
    lines = queue.Queue()
    threading.Thread(
        target=lambda: [lines.put(line) for line in watch.stderr], daemon=True
    ).start()
    try:
        assert "egraph: watch: watching " in lines.get(timeout=120)
        ran = system.egraph("install", "--yes", "app-misc/slow")
        assert ran.returncode == 0, ran.stdout + ran.stderr
        path = os.path.join(
            system.playground.eroot, "var", "lib", "egraph", "builds.jsonl"
        )
        deadline = time.monotonic() + 60
        while not os.path.exists(path):
            assert watch.poll() is None and time.monotonic() < deadline
            time.sleep(0.2)
        watch.send_signal(signal.SIGTERM)
        assert watch.wait(timeout=30) == 0
    finally:
        if watch.poll() is None:
            watch.kill()
            watch.wait()
    (record,) = history(system)
    assert record["origin"] == "emerge"
    assert record["run"].startswith("emerge-")
    assert (record["cpv"], record["repo"], record["slot"]) == (
        "app-misc/slow-1",
        "test_repo",
        "0",
    )
    assert record["outcome"] == "merged"
    assert record["build_seconds"] >= 6
    phases = {p["phase"]: p["seconds"] for p in record["phases"]}
    assert phases.get("compile", 0) >= 3
    assert sum(phases.values()) <= record["build_seconds"] + 0.01
    assert record["merge_seconds"] >= 0
    with open(os.path.join(vdb_path(system), "SIZE")) as f:
        assert record["installed_size"] == int(f.read())


def vdb_path(system):
    return os.path.join(
        system.playground.eroot, "var", "db", "pkg", "app-misc", "slow-1"
    )
