"""egraph refreshing its store through egraph-build as the system changes."""

import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading

import portage
import pytest

from egraph_build import build, installed
from test_build import add_package, add_to_world, age, fresh_vardb, vdb

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

BUILDER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTAGE_LIB = os.path.dirname(os.path.dirname(portage.__file__))


def scenario_system(mutable_playground, tmp_path, name):
    playground = mutable_playground(name)
    age(playground.eroot)
    log = tmp_path / "builds"
    builder = tmp_path / "egraph-build"
    builder.write_text(
        "#!/bin/sh\n"
        f'echo "$@" >> "{log}"\n'
        f'PYTHONPATH="{BUILDER_DIR}:{PORTAGE_LIB}" exec "{sys.executable}" -m egraph_build "$@"\n'
    )
    builder.chmod(0o755)
    return playground, tmp_path / "installed.egraph", builder, log


@pytest.fixture
def system(mutable_playground, tmp_path):
    return scenario_system(mutable_playground, tmp_path, "reference")


@pytest.fixture
def updates_system(mutable_playground, tmp_path):
    return scenario_system(mutable_playground, tmp_path, "updates")


def egraph(system, *args):
    playground, store, builder, _ = system
    return subprocess.run(
        [
            EGRAPH,
            "--store",
            str(store),
            "--config-root",
            playground.eroot,
            "--eprefix",
            playground.eprefix,
            "--builder",
            str(builder),
            *args,
        ],
        capture_output=True,
        text=True,
        # The playground's environment, which names its repositories.
        env=dict(playground.settings.environ(), EGRAPH_STRICT="1"),
    )


def query(system, *extra):
    return egraph(system, *extra, "export", "--format", "json")


def expected(playground):
    return installed.to_json(build.full(fresh_vardb(playground)).layer)


def builds(system):
    """The store builds, apart from the repository index's (and the notices' reads)."""
    log = system[3]
    lines = log.read_text().splitlines() if log.exists() else []
    return [
        line for line in lines if not line.startswith(("--repository", "--notices"))
    ]


def test_first_query_builds_the_store(system):
    playground = system[0]
    result = query(system)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected(playground)
    assert len(builds(system)) == 1


def test_fresh_store_is_not_rebuilt(system):
    query(system)
    result = query(system)
    assert result.returncode == 0, result.stderr
    assert len(builds(system)) == 1


@pytest.mark.parametrize(
    "change",
    [
        lambda p: add_package(p, "dev-libs/alt-b-1"),
        lambda p: shutil.rmtree(vdb(p, "dev-libs/cond-1")),
        lambda p: add_package(p, "sys-apps/new-1", RDEPEND="dev-libs/lib:2"),
        lambda p: add_to_world(p, "app-misc/old"),
    ],
    ids=["added", "removed", "new-category", "world"],
)
def test_changes_are_picked_up(system, change):
    playground = system[0]
    query(system)
    change(playground)
    result = query(system)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected(playground)
    assert len(builds(system)) == 2
    assert builds(system)[-1].startswith("--incremental --store ")


def test_no_refresh_answers_from_the_stale_store(system):
    playground = system[0]
    before = query(system).stdout
    add_package(playground, "dev-libs/alt-b-1")
    result = query(system, "--no-refresh")
    assert result.returncode == 0
    assert result.stdout == before
    assert "warning: answering from a stale store" in result.stderr
    assert len(builds(system)) == 1


def test_rebuild_writes_a_full_store(system):
    result = egraph(system, "rebuild")
    assert result.returncode == 0, result.stderr
    assert builds(system)[-1].startswith("--full --store ")
    assert query(system).stdout == expected(system[0])


def test_check_finds_no_drift_in_a_current_store(system):
    query(system)
    result = egraph(system, "check")
    assert (result.returncode, result.stdout, result.stderr) == (0, "", "")


def test_check_reports_what_the_store_missed(system):
    playground = system[0]
    query(system)
    add_package(playground, "dev-libs/alt-b-1")
    shutil.rmtree(vdb(playground, "dev-libs/nocond-1"))
    result = egraph(system, "check")
    assert result.returncode == 4
    # alt-b-1 is new, nocond-1 is gone, and root-1's || now matches alt-b-1.
    assert result.stdout.splitlines() == [
        "~app-misc/root-1",
        "+dev-libs/alt-b-1",
        "-dev-libs/nocond-1",
    ]


def test_refresh_brings_the_store_up_to_date_and_prints_nothing(system):
    playground = system[0]
    result = egraph(system, "refresh")
    assert (result.returncode, result.stdout) == (0, "")
    assert len(builds(system)) == 1
    assert len(index_builds(system)) == 1
    # Current: nothing to do.
    assert egraph(system, "refresh").returncode == 0
    assert len(builds(system)) == 1
    add_package(playground, "dev-libs/alt-b-1")
    assert egraph(system, "refresh").returncode == 0
    assert builds(system)[-1].startswith("--incremental --store ")
    assert query(system, "--no-refresh").stdout == expected(playground)


def start_watch(system):
    """egraph watch on the system's store, and a queue of its log lines."""
    playground, store, builder, _ = system
    process = subprocess.Popen(
        [
            EGRAPH,
            "--store",
            str(store),
            "--config-root",
            playground.eroot,
            "--eprefix",
            playground.eprefix,
            "--builder",
            str(builder),
            "watch",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        # Its log to the playground's file, never the journal.
        env=dict(playground.settings.environ(), EGRAPH_STRICT="1", EGRAPH_LOG="file"),
    )
    lines = queue.Queue()
    threading.Thread(
        target=lambda: [lines.put(line) for line in process.stderr], daemon=True
    ).start()
    return process, lines


def stop(process):
    if process.poll() is None:
        process.kill()
        process.wait()


def test_watch_refreshes_once_a_change_settles_and_stops_on_sigterm(system):
    """egraph watch builds the stores, refreshes them after a merge lands, and ends cleanly on
    SIGTERM."""
    playground = system[0]
    process, lines = start_watch(system)
    try:
        assert "egraph: watch: watching " in lines.get(timeout=120)
        assert len(builds(system)) == 1
        add_package(playground, "dev-libs/alt-b-1")
        assert "egraph: watch: refreshed in " in lines.get(timeout=120)
        assert builds(system)[-1].startswith("--incremental --store ")
        # Current: a query has nothing to build.
        assert query(system).stdout == expected(playground)
        assert len(builds(system)) == 2
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=30) == 0
    finally:
        stop(process)


def status(system):
    path = system[1].parent / "status.json"
    return json.loads(path.read_text()) if path.exists() else None


def settings(system, text):
    path = os.path.join(system[0].eroot, "etc/egraph/egraph.conf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def test_watch_writes_the_plan_to_the_status_file_after_each_refresh(updates_system):
    system = updates_system
    playground = system[0]
    repository, other = sorted(playground.settings.repositories, key=lambda r: r.name)
    os.makedirs(os.path.join(repository.location, "metadata"), exist_ok=True)
    with open(os.path.join(repository.location, "metadata/timestamp.chk"), "w") as f:
        f.write("Tue, 06 Oct 2026 18:46:01 +0000\n")
    process, lines = start_watch(system)
    try:
        assert "egraph: watch: watching " in lines.get(timeout=120)
        first = status(system)
        assert first["command"] == "updates --world -D -N --held"
        updates = egraph(system, *first["command"].split())
        assert first["lines"] == updates.stdout.splitlines() != []
        assert sorted(first["repositories"], key=lambda r: r["name"]) == [
            {"name": repository.name, "synced": 1791312361},
            {"name": other.name},
        ]
        assert first["counts"]["upgrades"] == sum(
            line.split("\t")[1] == "upgrade" for line in first["lines"]
        )
        add_package(playground, "dev-libs/alt-b-1")
        assert "egraph: watch: refreshed in " in lines.get(timeout=120)
        second = status(system)
        assert (
            second["stores"]["installed"]["built"]
            > first["stores"]["installed"]["built"]
        )
        assert (
            second["lines"]
            == egraph(system, *first["command"].split()).stdout.splitlines()
        )
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=30) == 0
    finally:
        stop(process)


def notices(system):
    path = system[1].parent / "notices.json"
    return json.loads(path.read_text()) if path.exists() else None


def logged_notices(system):
    path = os.path.join(system[0].eprefix, "var", "log", "egraph.log")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        events = [json.loads(line) for line in f]
    return [e for e in events if e["event"] == "notice"]


def test_watch_writes_the_notices_and_logs_new_ones(system):
    """notices.json holds what notices lists, kept up as the system changes, whatever plan says;
    a notice new since the last file is logged once."""
    settings(system, "plan = never\n")
    process, lines = start_watch(system)
    try:
        assert "egraph: watch: watching " in lines.get(timeout=120)
        first = notices(system)
        assert first is not None
        assert status(system) is None
        keys = [notice["key"] for notice in first["notices"]]
        listed = egraph(system, "notices").stdout.splitlines()
        assert keys == [
            f"masked:{row.split()[0]}"
            for row in listed
            if row.split("\t")[1] == "masked"
        ]
        assert logged_notices(system) == []
        add_package(
            system[0],
            "dev-libs/needy-1",
            KEYWORDS="x86",
            REQUIRES="x86_64: libgone.so.9",
        )
        assert "egraph: watch: refreshed in " in lines.get(timeout=120)
        second = notices(system)
        assert second["stores"] != first["stores"]
        added = [n for n in second["notices"] if n["key"] not in keys]
        assert [(n["key"], n["detail"], n["since"]) for n in added] == [
            ("missing:dev-libs/needy-1", ["libgone.so.9 (x86_64)"], second["written"])
        ]
        kept = {n["key"]: n["since"] for n in second["notices"] if n["key"] in keys}
        assert kept == {n["key"]: n["since"] for n in first["notices"]}
        assert [
            (e["key"], e["notice"], e["priority"]) for e in logged_notices(system)
        ] == [("missing:dev-libs/needy-1", "missing", 4)]
    finally:
        stop(process)


def test_watch_plans_after_merges_only_as_the_settings_say(system):
    """plan = sync leaves the plan of before a merge; plan = never makes none."""
    settings(system, "plan = sync\n")
    process, lines = start_watch(system)
    try:
        assert "egraph: watch: watching " in lines.get(timeout=120)
        before = status(system)
        assert before is not None
        add_package(system[0], "dev-libs/alt-b-1")
        assert "egraph: watch: refreshed in " in lines.get(timeout=120)
        assert status(system) == before
    finally:
        stop(process)
    os.remove(system[1].parent / "status.json")
    settings(system, "plan = never\n")
    process, lines = start_watch(system)
    try:
        assert "egraph: watch: watching " in lines.get(timeout=120)
        assert status(system) is None
    finally:
        stop(process)


def test_a_refresh_right_after_a_change_writes_a_store_that_stays_current(system):
    """As the post_emerge hook refreshes: the change is under a second old, so the refresh
    waits until it is not, and the next query trusts the store rather than refresh again.
    """
    playground = system[0]
    assert egraph(system, "refresh").returncode == 0
    add_package(playground, "dev-libs/alt-b-1")
    assert egraph(system, "refresh").returncode == 0
    assert len(builds(system)) == 2
    assert query(system).stdout == expected(playground)
    assert len(builds(system)) == 2


@pytest.fixture
def repository_system(mutable_playground, tmp_path):
    """The repository scenario, whose www-apps cps are in no store until asked for."""
    return scenario_system(mutable_playground, tmp_path, "repository")


def in_repository(system, *args):
    """egraph in the playground's environment, which names its repositories."""
    playground, store, builder, _ = system
    return subprocess.run(
        [EGRAPH, "--store", str(store), "--builder", str(builder), *args],
        capture_output=True,
        text=True,
        env=dict(playground.settings.environ(), EGRAPH_STRICT="1"),
    )


def evaluations(system):
    log = system[3]
    lines = log.read_text().splitlines() if log.exists() else []
    return [line for line in lines if line.startswith("--evaluate")]


def export_index(system):
    return in_repository(system, "export", "--format", "json", "--repository")


def index_json(system):
    """The index as the builder prints it in the playground's environment."""
    playground, _, builder, _ = system
    return subprocess.run(
        [str(builder), "--repository-json"],
        capture_output=True,
        text=True,
        check=True,
        env=playground.settings.environ(),
    ).stdout


def index_builds(system):
    log = system[3]
    lines = log.read_text().splitlines() if log.exists() else []
    return [line for line in lines if line.split()[0] == "--repository"]


def test_the_repository_index_is_built_on_first_use_and_kept_while_current(
    repository_system,
):
    result = export_index(repository_system)
    assert result.returncode == 0, result.stderr
    assert result.stdout == index_json(repository_system)
    assert '"www-apps/unused"' in result.stdout
    assert export_index(repository_system).stdout == result.stdout
    assert len(index_builds(repository_system)) == 1


def test_a_configuration_change_rebuilds_the_repository_index(repository_system):
    playground = repository_system[0]
    export_index(repository_system)
    path = os.path.join(playground.eroot, "etc/portage/package.accept_keywords")
    with open(path, "a") as f:
        f.write("app-misc/testing ~x86\n")
    result = export_index(repository_system)
    assert result.returncode == 0, result.stderr
    assert result.stdout == index_json(repository_system)
    assert '"app-misc/testing"' in result.stdout
    assert len(index_builds(repository_system)) == 2


def test_plan_evaluates_a_cp_only_the_repositories_know(repository_system):
    result = in_repository(repository_system, "plan", "www-apps/unused")
    assert result.returncode == 0, result.stderr
    merges = {
        line.split("\t")[2]
        for line in result.stdout.splitlines()
        if line.split("\t")[1] != "masked"
    }
    assert merges == {"www-apps/unused-1", "www-apps/helper-1"}
    assert len(evaluations(repository_system)) == 1
    assert "www-apps/unused" in evaluations(repository_system)[0].split()
    # Kept: the next plan needs no builder run.
    again = in_repository(repository_system, "plan", "unused")
    assert again.returncode == 0, again.stderr
    assert again.stdout == result.stdout
    assert len(evaluations(repository_system)) == 1


def test_plan_without_refresh_does_not_evaluate(repository_system):
    assert in_repository(repository_system, "stats").returncode == 0
    result = in_repository(repository_system, "--no-refresh", "plan", "www-apps/unused")
    assert result.returncode == 1
    assert result.stderr == (
        "egraph: plan: www-apps/unused: not evaluated yet, and --no-refresh keeps "
        "egraph-build from evaluating it\n"
    )
    assert evaluations(repository_system) == []


def test_plan_refuses_a_name_no_repository_knows(repository_system):
    result = in_repository(repository_system, "plan", "nowhere")
    assert result.returncode == 1
    assert "nowhere: no package by that name in the repositories" in result.stderr
    assert evaluations(repository_system) == []


def on_terminal(command, answer, env):
    """Runs command on a pseudo-terminal, answering its [y/N] question with answer, or its
    questions in turn with a list of answers: (status, everything it printed, ANSI codes
    stripped)."""
    import pty
    import re
    import select

    master, slave = pty.openpty()
    process = subprocess.Popen(
        command, stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True
    )
    os.close(slave)
    printed = b""
    answers = [answer] if isinstance(answer, str) else list(answer)
    answered = 0
    while True:
        ready, _, _ = select.select([master], [], [], 120)
        if not ready:
            process.kill()
            break
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        printed += chunk
        while answered < min(printed.count(b"[y/N]"), len(answers)):
            os.write(master, answers[answered].encode() + b"\n")
            answered += 1
    os.close(master)
    status = process.wait()
    text = re.sub(r"\x1b\[[0-9;]*m", "", printed.decode(errors="replace"))
    return status, text.replace("\r\n", "\n")


@pytest.mark.parametrize("answer", ["y", "n"])
def test_use_changes_are_written_on_yes_and_planned_again(
    mutable_playground, tmp_path, answer
):
    """On a terminal, plan offers to write the USE changes it needs to package.use; on yes it
    writes them and plans again on stores refreshed for them, which need none."""
    playground = mutable_playground("usechange")
    age(playground.eroot)
    builder = tmp_path / "egraph-build"
    builder.write_text(
        "#!/bin/sh\n"
        f'PYTHONPATH="{BUILDER_DIR}:{PORTAGE_LIB}" exec "{sys.executable}" -m egraph_build "$@"\n'
    )
    builder.chmod(0o755)
    command = [
        EGRAPH,
        "--store",
        str(tmp_path / "installed.egraph"),
        "--config-root",
        playground.eroot,
        "--eprefix",
        playground.eprefix,
        "--builder",
        str(builder),
        "--color",
        "never",
        "plan",
        "app-misc/wantgtk",
    ]
    status, printed = on_terminal(command, answer, dict(os.environ, EGRAPH_STRICT="1"))
    path = os.path.join(playground.eroot, "etc", "portage", "package.use")
    assert ">=dev-libs/lib-2 gtk\n" in printed
    assert f"Write the USE changes to {path}? [y/N]" in printed
    if answer == "n":
        assert status == 6, printed
        assert not os.path.exists(path)
        return
    assert status == 0, printed
    with open(path) as written:
        assert written.read() == (
            "# required by app-misc/wantgtk-1::test_repo\n"
            "# required by app-misc/wantgtk (argument)\n"
            ">=dev-libs/lib-2 gtk\n"
        )
    again = printed.split("planning again.\n", 1)[1]
    assert "USE changes needed" not in again
    assert "dev-libs/gtkdep" in again


def test_a_builder_from_another_version_is_named(system, tmp_path):
    """A store egraph-build writes in another format than egraph reads is put down to the two
    being from different versions; one kept from before says how to replace it."""
    from egraph_build.store import FORMAT_VERSION

    playground, store, builder, log = system
    older = tmp_path / "older-egraph-build"
    older.write_text(
        "#!/bin/sh\n"
        f'"{builder}" "$@" || exit\n'
        f'"{sys.executable}" -c "import struct, sys\n'
        "with open(sys.argv[1], 'r+b') as f:\n"
        "    f.seek(8)\n"
        f"    f.write(struct.pack('<I', {FORMAT_VERSION - 1}))\n"
        f'" "{store}"\n'
    )
    older.chmod(0o755)
    ran = egraph((playground, store, older, log), "orphans")
    assert ran.returncode != 0
    assert (
        f"egraph and {older} are from different versions: {older} wrote {store} as an "
        f"egraph store of format {FORMAT_VERSION - 1}, but this egraph reads format "
        f"{FORMAT_VERSION}; install both from the same release"
    ) in ran.stderr
    kept = egraph(system, "--no-refresh", "orphans")
    assert kept.returncode != 0
    assert (
        f"{store} is an egraph store of format {FORMAT_VERSION - 1}, from another egraph "
        "version"
    ) in kept.stderr
    assert egraph(system, "orphans").returncode == 0
