"""egraph exec: a plan carried out by egraph-build --worker leaves the system egraph install, with
the real emerge, leaves."""

import glob
import json
import os
import shutil
import subprocess
import time

import pytest

from test_actions import EGRAPH, System
from test_build import age
from test_worker import EBUILDS, INSTALL, PLAIN, Machine, differences

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def over(ebuilds, tmp_path, **config):
    """A playground with ebuilds, as an egraph System and a Machine over it."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(ebuilds=ebuilds, **config)
    yield System(playground, tmp_path), Machine(playground, tmp_path)
    playground.cleanup()


@pytest.fixture
def machines(gnupg_home, tmp_path):
    """The worker's ebuilds."""
    yield from over(EBUILDS, tmp_path)


SLOW = INSTALL + "src_compile() { sleep 1; }\n"
# Each phase that holds the build directory appends a mark and the time; the compile waits.
MARKED = """S="${WORKDIR}"
mark() { mkdir -p "${EROOT}"/var/tmp; echo "$1 $(date +%s.%N)" >> "${EROOT}"/var/tmp/marks; }
pkg_pretend() { mark pretend; }
pkg_setup() { mark setup; }
src_compile() { sleep 8; }
src_install() { insinto /usr/share/${PN}; echo x > "${T}"/x; doins "${T}"/x; }
pkg_postinst() { mark postinst; }
"""
# base, side and also build at once; mid needs base merged to build, top all three.
PARALLEL = {
    "app-misc/base-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": SLOW},
    "app-misc/side-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": SLOW},
    "app-misc/also-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": SLOW},
    "app-misc/mid-1": {
        **PLAIN,
        "IUSE": "doc",
        "DEPEND": "app-misc/base",
        "MISC_CONTENT": SLOW
        + 'pkg_setup() { has_version app-misc/base || die "base not seen"; }\n',
    },
    "app-misc/top-1": {
        **PLAIN,
        "IUSE": "doc",
        "RDEPEND": "app-misc/mid app-misc/side app-misc/also",
        "MISC_CONTENT": SLOW,
    },
    "app-misc/marked-1": {**PLAIN, "MISC_CONTENT": MARKED},
}
NEEDS = {
    "app-misc/mid-1": ["app-misc/base-1"],
    "app-misc/top-1": ["app-misc/mid-1", "app-misc/side-1", "app-misc/also-1"],
}


@pytest.fixture
def parallel(gnupg_home, tmp_path):
    """The parallel ebuilds, with two jobs configured, whatever the free space."""
    options = "--jobs=2 --jobs-tmpdir-require-free-gb=0"
    config = {"make.conf": (f'EMERGE_DEFAULT_OPTS="{options}"',)}
    yield from over(PARALLEL, tmp_path, user_config=config)


def forget_stores(system):
    for path in (system.store, system.store.with_suffix(".evaluated.egraph")):
        if path.exists():
            path.unlink()


def carried_out_as_install(system, machine, before, args, command=None):
    """After the cpvs before, merged by emerge -1: egraph install and egraph exec, given args and
    --yes, leave the same system. exec's run."""
    for cpv in before:
        machine.emerge(f"-1 ={cpv}")
    age(system.playground.eroot)
    machine.save()
    installed = system.egraph("install", "--yes", *args)
    assert installed.returncode == 0, installed.stdout + installed.stderr
    emerged = machine.state()
    machine.restore()
    forget_stores(system)
    age(system.playground.eroot)
    ran = system.egraph(*(command or ["exec"]), "--yes", *args)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    worked = machine.state()
    assert worked == emerged, differences(emerged, worked)
    return ran


@pytest.mark.parametrize(
    "before, args, facts",
    [
        ([], ["app-misc/needs"], {"var/lib/portage/world": "app-misc/needs\n"}),
        (
            [],
            ["-1", "app-misc/needs"],
            {"var/lib/needs/postinst": "needs-1 after nothing\n"},
        ),
        (
            ["app-misc/files-1"],
            ["-u", "app-misc/files"],
            {"usr/share/files/files.conf": None},
        ),
        (
            ["app-misc/old-1"],
            ["app-misc/new"],
            {"usr/share/shared/data": "new\n", "var/db/pkg/app-misc/old-1": None},
        ),
    ],
    ids=["pulled-in", "oneshot", "replacing", "blocker"],
)
def test_exec_merges_as_install_does(machines, before, args, facts):
    """The image, vdb entries and world file: a package and the dependency it pulls in, the
    same without joining the world file, an update replacing an installed version, and a
    package whose blocker uninstalls one it takes a file over from."""
    system, machine = machines
    ran = carried_out_as_install(system, machine, before, args)
    for path, content in facts.items():
        if content is None:
            assert not os.path.lexists(machine.path(path)), path
        else:
            with open(machine.path(path)) as f:
                assert f.read() == content, path
    # A line per event, each request ending done.
    assert ran.stdout.count("\tmerged") + ran.stdout.count("\tuninstalled") >= 1


def test_egraph_exec_is_exec(machines, tmp_path):
    """Through a link named egraph-exec, the global options among the command's own."""
    system, machine = machines
    link = tmp_path / "egraph-exec"
    link.symlink_to(os.path.abspath(EGRAPH))
    age(system.playground.eroot)
    command = system.command("--yes", "app-misc/lib")
    ran = subprocess.run(
        [str(link), *command[1:]],
        capture_output=True,
        text=True,
        env=dict(os.environ, EGRAPH_STRICT="1"),
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert machine.installed("app-misc/lib-1")


def test_a_run_is_logged_to_the_journal_as_to_the_file(machines):
    """With --log both, the journal holds the run's events, as the file does, under its id."""
    system, machine = machines
    if not os.path.isdir("/run/systemd/system") or not shutil.which("journalctl"):
        pytest.skip("systemd does not run here")
    age(system.playground.eroot)
    ran = system.egraph("--log", "both", "exec", "--yes", "-1", "app-misc/lib")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    if "cannot log to the journal" in ran.stdout:
        pytest.skip("egraph is built without the journal")
    events = logged(system)
    run = events[0]["run"]
    deadline = time.monotonic() + 10
    while True:
        journal = subprocess.run(
            [
                "journalctl",
                "--no-pager",
                "-o",
                "json",
                "-t",
                "egraph",
                f"EGRAPH_RUN={run}",
            ],
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        if len(journal) >= len(events) or time.monotonic() > deadline:
            break
        time.sleep(0.2)
    entries = [json.loads(line) for line in journal]
    assert [entry["EGRAPH_EVENT"] for entry in entries] == [e["event"] for e in events]
    assert [entry["MESSAGE"] for entry in entries] == [e["message"] for e in events]
    assert entries[-1]["EGRAPH_STATUS"] == "ok"


def test_a_failed_build_stops_the_run(machines):
    """The step that failed is named with its phase and log; nothing after it runs."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/broken", "app-misc/lib")
    assert ran.returncode != 0
    assert "app-misc/broken-1: compile failed with status 1 (log: " in ran.stderr
    assert not machine.installed("app-misc/broken-1")
    first = ran.stdout.splitlines()[0].split("\t")
    if first[1] == "app-misc/broken-1":
        assert not machine.installed("app-misc/lib-1")


def read_trace(path):
    """The trace's lines as (what, cpv), in order, each time after the one before."""
    lines = [line.split("\t") for line in path.read_text().splitlines()]
    times = [float(time) for time, _, _ in lines]
    assert times == sorted(times)
    return [(what, cpv) for _, what, cpv in lines]


def builds_at_once(trace):
    """The most builds running at once."""
    running = most = 0
    for what, _ in trace:
        if what == "build-start":
            running += 1
            most = max(most, running)
        elif what in ("built", "build-failed"):
            running -= 1
    return most


def logged(system):
    """The events exec logged to its playground's file, as egraph's own log (meson test sets
    EGRAPH_LOG=file)."""
    path = os.path.join(system.playground.eprefix, "var", "log", "egraph.log")
    with open(path) as f:
        return [json.loads(line) for line in f]


# What each trace line of a step's end is logged as.
LOGGED_AS = {
    "built": "built",
    "merged": "merged",
    "uninstalled": "uninstalled",
    "build-failed": "failed",
    "merge-failed": "failed",
    "uninstall-failed": "failed",
    "skipped": "skipped",
}


def holds_the_trace(events, trace):
    """The log of one run: begun and ended once, under one id, each step's end as the trace has
    it, phases only between a build's start and end, and the end's counts its events'.
    """
    assert events[0]["event"] == "run" and events[-1]["event"] == "end"
    assert len({event["run"] for event in events}) == 1
    ends = [
        (e["event"], e["cpv"])
        for e in events
        if e["event"] not in ("run", "phase", "end")
    ]
    assert sorted(ends) == sorted(
        (LOGGED_AS[what], cpv) for what, cpv in trace if what in LOGGED_AS
    )
    end = events[-1]
    for kind in ("merged", "uninstalled", "failed", "skipped"):
        assert end[kind] == sum(1 for event, _ in ends if event == kind), kind
    started = {cpv for what, cpv in trace if what == "build-start"}
    assert {e["cpv"] for e in events if e["event"] == "phase"} <= started


def holds_the_rules(trace, jobs):
    """emerge's scheduling with merge-wait: never two merges at once, nor a merge beside a
    build, at most jobs builds, and no build before what it needs is merged."""
    building = merging = 0
    for what, cpv in trace:
        if what == "build-start":
            assert merging == 0, (what, cpv)
            building += 1
        elif what in ("built", "build-failed"):
            building -= 1
        elif what == "merge-start":
            assert merging == 0 and building == 0, (what, cpv)
            merging += 1
        elif what == "uninstall-start":
            assert merging == 0, (what, cpv)
            merging += 1
        elif what != "skipped":
            merging -= 1
    assert building == merging == 0
    assert builds_at_once(trace) <= jobs
    for cpv, needed in NEEDS.items():
        if ("build-start", cpv) in trace:
            started = trace.index(("build-start", cpv))
            for other in needed:
                assert trace.index(("merged", other)) < started, (cpv, other)


def without_counters(state):
    """The merge order sets each vdb entry's COUNTER, and parallel runs merge in the order
    builds finish."""
    vdb = {}
    for key, value in state["vdb"].items():
        if key.endswith("/COUNTER"):
            continue
        if key.endswith("/metadata"):
            value = [line for line in value if not line.startswith("COUNTER=")]
        vdb[key] = value
    return {**state, "vdb": vdb}


def test_a_parallel_exec_holds_emerges_rules_and_merges_as_install_does(
    parallel, tmp_path
):
    """With EMERGE_DEFAULT_OPTS' two jobs, as emerge would run them."""
    system, machine = parallel
    machine.save()
    installed = system.egraph("install", "--yes", "app-misc/top")
    assert installed.returncode == 0, installed.stdout + installed.stderr
    emerged = without_counters(machine.state())
    machine.restore()
    forget_stores(system)
    age(system.playground.eroot)
    trace = tmp_path / "trace"
    ran = system.egraph("exec", "--yes", "--trace", str(trace), "app-misc/top")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    worked = without_counters(machine.state())
    assert worked == emerged, differences(emerged, worked)
    traced = read_trace(trace)
    holds_the_rules(traced, 2)
    assert builds_at_once(traced) == 2
    assert [cpv for what, cpv in traced if what == "merged"][-1] == "app-misc/top-1"
    # Each build's output only in its log.
    assert "installing to stdout" not in ran.stderr


def test_exec_j_overrides_the_configured_jobs(parallel, tmp_path):
    system, machine = parallel
    age(system.playground.eroot)
    trace = tmp_path / "trace"
    ran = system.egraph("exec", "--yes", "-j3", "--trace", str(trace), "app-misc/top")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    traced = read_trace(trace)
    holds_the_rules(traced, 3)
    assert builds_at_once(traced) == 3
    assert machine.installed("app-misc/top-1")


def test_builds_wait_to_run_alone_without_room_in_the_build_directory(
    parallel, tmp_path
):
    """As emerge's --jobs-tmpdir-require-free-gb holds them back."""
    system, machine = parallel
    age(system.playground.eroot)
    trace = tmp_path / "trace"
    options = "--jobs=2 --jobs-tmpdir-require-free-gb=1048576"
    ran = subprocess.run(
        system.command("exec", "--yes", "-j3", "--trace", str(trace), "app-misc/top"),
        capture_output=True,
        text=True,
        env=dict(os.environ, EGRAPH_STRICT="1", EMERGE_DEFAULT_OPTS=options),
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    traced = read_trace(trace)
    holds_the_rules(traced, 3)
    assert builds_at_once(traced) == 1
    assert "builds wait to run alone" in ran.stdout


def test_each_build_takes_a_jobserver_token(parallel, tmp_path):
    """Under FEATURES=jobserver-token, a token from MAKEFLAGS' jobserver per build, the first
    held already when MAKEFLAGS comes from the environment, as emerge takes them; each given
    back."""
    system, machine = parallel
    age(system.playground.eroot)
    fifo = tmp_path / "jobserver"
    os.mkfifo(fifo)
    jobserver = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
    try:
        os.write(jobserver, b"+")
        trace = tmp_path / "trace"
        environment = dict(
            os.environ,
            EGRAPH_STRICT="1",
            FEATURES="jobserver-token",
            MAKEFLAGS=f"--jobserver-auth=fifo:{fifo}",
        )
        ran = subprocess.run(
            system.command(
                "exec", "--yes", "-j3", "--trace", str(trace), "app-misc/top"
            ),
            capture_output=True,
            text=True,
            env=environment,
        )
        assert ran.returncode == 0, ran.stdout + ran.stderr
        traced = read_trace(trace)
        holds_the_rules(traced, 3)
        assert builds_at_once(traced) == 2
        assert os.read(jobserver, 2) == b"+"
    finally:
        os.close(jobserver)


def test_a_concurrent_emerge_waits_on_the_build_directory_lock(parallel, tmp_path):
    """emerge, run while exec builds the same package, waits for portage's lock on the build
    directory, which its pkg_pretend takes, until exec's merge lets it go."""
    system, machine = parallel
    age(system.playground.eroot)
    trace = tmp_path / "trace"
    running = subprocess.Popen(
        system.command("exec", "--yes", "--trace", str(trace), "app-misc/marked"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=dict(os.environ, EGRAPH_STRICT="1"),
    )
    try:
        deadline = time.monotonic() + 60
        while not (trace.exists() and "build-start" in trace.read_text()):
            assert running.poll() is None and time.monotonic() < deadline
            time.sleep(0.1)
        launched = time.time()
        machine.emerge("-1 =app-misc/marked-1")
    finally:
        out, err = running.communicate()
    assert running.returncode == 0, out + err
    with open(machine.path("var/tmp/marks")) as f:
        marks = [line.split() for line in f]
    assert [name for name, _ in marks] == [
        "pretend",
        "setup",
        "postinst",
        "pretend",
        "setup",
        "postinst",
    ]
    # Started long before exec merged, emerge went on only once it had.
    merged = float(marks[2][1])
    assert merged - launched > 4
    assert float(marks[3][1]) > merged


@pytest.fixture
def observed(gnupg_home, tmp_path):
    """held, whose compile waits for a file to appear, and quick, under FEATURES=observability
    (for emerge) with two jobs: quick built and waiting to merge while held compiles. The
    file's path.
    """
    go = tmp_path / "go"
    ebuilds = {
        "app-misc/held-1": {
            **PLAIN,
            "IUSE": "doc",
            "MISC_CONTENT": INSTALL
            + f"src_compile() {{ while [[ ! -e {go} ]]; do sleep 0.1; done; }}\n",
        },
        "app-misc/quick-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": INSTALL},
    }
    options = "--jobs=2 --jobs-tmpdir-require-free-gb=0"
    config = {
        "make.conf": (
            f'EMERGE_DEFAULT_OPTS="{options}"',
            'FEATURES="${FEATURES} observability"',
        )
    }
    for system, machine in over(ebuilds, tmp_path, user_config=config):
        yield system, machine, go


# Where emerge publishes, and where egraph exec does.
EMERGES = ("portage", "emerge-")
RUNS = ("egraph", "exec-")


def published(eprefix, where):
    """The status files under eprefix where says, each as (pid in its name, snapshot)."""
    directory, prefix = where
    found = []
    for path in glob.glob(os.path.join(eprefix, "run", directory, prefix + "*.json")):
        try:
            with open(path) as f:
                snapshot = json.load(f)
        except (OSError, ValueError):
            continue
        found.append(
            (int(os.path.basename(path)[len(prefix) : -len(".json")]), snapshot)
        )
    return found


def snapshot_while_held(system, go, where, *args, **environment):
    """The snapshot egraph's args publish where says once held compiles and quick waits to
    merge, then the run let go; also whether its status file is left behind, and whether any
    was published in the other place meanwhile."""
    elsewhere = RUNS if where == EMERGES else EMERGES
    running = subprocess.Popen(
        system.command(*args, "--yes", "app-misc/held", "app-misc/quick"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=dict(os.environ, EGRAPH_STRICT="1", **environment),
    )
    strays = False
    try:
        deadline = time.monotonic() + 120
        caught = None
        while caught is None:
            strays = strays or bool(published(system.playground.eprefix, elsewhere))
            for pid, snapshot in published(system.playground.eprefix, where):
                tasks = {task["cpv"]: task for task in snapshot["tasks"]}
                held = tasks.get("app-misc/held-1", {})
                quick = tasks.get("app-misc/quick-1", {})
                if held.get("phase") == "compile" and quick.get("merge_wait"):
                    caught = pid, snapshot
            assert running.poll() is None and time.monotonic() < deadline
            time.sleep(0.1)
    finally:
        go.touch()
        out, err = running.communicate()
    assert running.returncode == 0, out + err
    assert not strays
    return caught, published(system.playground.eprefix, where)


# A snapshot's times and pids, which no two runs share: compared by whether each is there.
VARYING = {"pid", "start_time", "elapsed", "build_elapsed", "timestamp"}


def comparable(caught):
    pid, snapshot = caught
    assert snapshot["emerge_pid"] == pid

    def kept(entry):
        return {
            key: (value is None) if key in VARYING else value
            for key, value in entry.items()
            if key != "emerge_pid"
        }

    return {
        **kept(snapshot),
        "tasks": sorted((kept(task) for task in snapshot["tasks"]), key=str),
    }


def test_exec_publishes_its_status_as_emerge_does(observed):
    """exec's status file, in its own place and whatever FEATURES says, says what emerge's says
    of the same run at the same point under FEATURES=observability: one package compiling, one
    built and waiting to merge, the jobs counted alike; and it goes with the run."""
    system, machine, go = observed
    age(system.playground.eroot)
    machine.save()
    emerged, left = snapshot_while_held(system, go, EMERGES, "install")
    assert left == []
    machine.restore()
    forget_stores(system)
    go.unlink()
    age(system.playground.eroot)
    worked, left = snapshot_while_held(
        system, go, RUNS, "exec", FEATURES="-observability"
    )
    assert left == []
    assert comparable(worked) == comparable(emerged)


FAILS = INSTALL + 'src_compile() { die "fails on purpose"; }\n'
# fails and lib-2 fail to build: what needs them goes, what the installed lib-1 satisfies stays.
# broken is installed without what it needs; uses reaches it, building after fails2, which an
# installed fails2-0 still satisfies once fails2-1 fails.
DOC = {**PLAIN, "IUSE": "doc"}
KEEP_GOING = {
    "app-misc/fails-1": {**DOC, "MISC_CONTENT": FAILS},
    "app-misc/lib-1": {**DOC, "MISC_CONTENT": INSTALL},
    "app-misc/lib-2": {**DOC, "MISC_CONTENT": FAILS},
    "app-misc/user-1": {**DOC, "RDEPEND": "app-misc/lib", "MISC_CONTENT": INSTALL},
    "app-misc/user2-1": {
        **DOC,
        "RDEPEND": ">=app-misc/lib-2",
        "MISC_CONTENT": INSTALL,
    },
    "app-misc/needs-1": {**DOC, "DEPEND": "app-misc/fails", "MISC_CONTENT": INSTALL},
    "app-misc/top-1": {**DOC, "RDEPEND": "app-misc/needs", "MISC_CONTENT": INSTALL},
    "app-misc/indep-1": {**DOC, "MISC_CONTENT": INSTALL},
    "app-misc/broken-1": {**DOC, "RDEPEND": "app-misc/gone", "MISC_CONTENT": INSTALL},
    "app-misc/uses-1": {
        **DOC,
        "BDEPEND": "app-misc/fails2",
        "RDEPEND": "app-misc/broken",
        "MISC_CONTENT": INSTALL,
    },
    "app-misc/fails2-0": {**DOC, "MISC_CONTENT": INSTALL},
    "app-misc/fails2-1": {**DOC, "MISC_CONTENT": FAILS},
}


@pytest.fixture
def keep_going(gnupg_home, tmp_path):
    """The keep-going ebuilds, with --keep-going configured, whatever the free space."""
    options = "--keep-going --jobs-tmpdir-require-free-gb=0"
    config = {"make.conf": (f'EMERGE_DEFAULT_OPTS="{options}"',)}
    yield from over(KEEP_GOING, tmp_path, user_config=config)


def failed_as_install(system, machine, before, args, exec_args=()):
    """After the emerge commands before: egraph install and egraph exec, given args and --yes,
    each fail and leave the same system. exec's run."""
    for command in before:
        machine.emerge(command)
    age(system.playground.eroot)
    machine.save()
    installed = system.egraph("install", "--yes", *args)
    assert installed.returncode != 0, installed.stdout + installed.stderr
    emerged = without_counters(machine.state())
    machine.restore()
    forget_stores(system)
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", *exec_args, *args)
    assert ran.returncode != 0, ran.stdout + ran.stderr
    worked = without_counters(machine.state())
    assert worked == emerged, differences(emerged, worked)
    return ran


@pytest.mark.parametrize("jobs", ["1", "2"])
def test_keep_going_skips_what_emerge_drops(keep_going, tmp_path, jobs):
    """What needs a failed package, or what needs that, goes; what an installed version still
    satisfies stays, as emerge --keep-going resumes."""
    system, machine = keep_going
    trace = tmp_path / "trace"
    ran = failed_as_install(
        system,
        machine,
        ["-1 =app-misc/lib-1"],
        ["app-misc/top", "app-misc/user", "app-misc/user2", "app-misc/indep"],
        ["-j", jobs, "--trace", str(trace)],
    )
    for cpv in ("app-misc/lib-1", "app-misc/user-1", "app-misc/indep-1"):
        assert machine.installed(cpv), cpv
    traced = read_trace(trace)
    holds_the_rules(traced, int(jobs))
    skipped = {cpv for what, cpv in traced if what == "skipped"}
    assert skipped == {"app-misc/needs-1", "app-misc/top-1", "app-misc/user2-1"}
    events = logged(system)
    holds_the_trace(events, traced)
    assert events[-1]["status"] == "failed"
    failed = [event for event in events if event["event"] == "failed"]
    assert all(event["phase"] == "compile" and event["log"] for event in failed)
    assert "app-misc/user2-1\tskipped, needs >=app-misc/lib-2" in ran.stdout
    assert "app-misc/top-1\tskipped, needs app-misc/needs" in ran.stdout
    assert "; 1 more failed; 3 skipped" in ran.stderr


def test_keep_going_stops_where_emerge_cannot_resume(keep_going):
    """A merge left that reaches an installed package with a dependency nothing satisfies stops
    the run, as emerge refuses to resume."""
    system, machine = keep_going
    ran = failed_as_install(
        system,
        machine,
        ["-1 =app-misc/fails2-0", "-1 --nodeps =app-misc/broken-1"],
        ["-1", "app-misc/fails2", "app-misc/uses"],
    )
    assert not machine.installed("app-misc/uses-1")
    assert "cannot go on without app-misc/broken-1: app-misc/gone" in ran.stderr
