"""egraph tui in a real terminal: a private tmux server stands in for one."""

import json
import os
import shlex
import shutil
import subprocess
import time
from pathlib import Path

import pytest

from conftest import System, portdb, write_stores
from egraph_build import installed, repository
from egraph_build import store as egraph_store
from test_build import add_package, age, fresh_vardb
from test_refresh import builds, query, system  # noqa: F401 (a fixture)
from test_refresh import egraph as run_egraph
from test_refresh import updates_system  # noqa: F401 (a fixture)

EGRAPH = os.environ.get("EGRAPH")

pytestmark = [
    pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary"),
    pytest.mark.skipif(not shutil.which("tmux"), reason="needs tmux"),
]


def tmux(socket, *args):
    return subprocess.run(
        ["tmux", "-L", socket, "-f", "/dev/null", *args],
        capture_output=True,
        text=True,
        check=False,
    )


def wait_for(socket, *shown, gone=(), seconds=30):
    """The screen once it shows every text in shown and none in gone. A capture can land
    partway through a redraw, so everything a check reads is waited for."""
    deadline = time.monotonic() + seconds
    while True:
        screen = tmux(socket, "capture-pane", "-p", "-t", "t").stdout
        missing = [text for text in shown if text not in screen]
        left = [text for text in gone if text in screen]
        if not missing and not left:
            return screen
        if time.monotonic() >= deadline:
            raise AssertionError(
                f"never showed {missing!r}, still showed {left!r}:\n{screen}"
            )
        time.sleep(0.1)


def skip_without_tui():
    result = subprocess.run(
        [EGRAPH, "--store", "/nonexistent", "--no-refresh", "tui"], capture_output=True
    )
    if result.returncode == 3:
        pytest.skip("egraph was built without the terminal interface")


def test_tui_shows_the_store_and_quits(playgrounds, tmp_path):
    vardb = playgrounds("roots").vardb
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("roots"), path)
    skip_without_tui()

    socket = f"egraph-test-{os.getpid()}"
    command = f"{EGRAPH} --store {path} --no-refresh tui; echo EXIT=$?; sleep 30"
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "90", "-y", "16", command)
    try:
        packages = len(installed.build(vardb).installed())
        # It opens on the updates, of which there are none here.
        wait_for(socket, "nothing to update")
        tmux(socket, "send-keys", "-t", "t", "u")
        wait_for(socket, f"{packages} of {packages} packages")
        # Search, keep the search, open the one match.
        tmux(socket, "send-keys", "-t", "t", "/", "world", "Enter")
        wait_for(socket, "1 of")
        tmux(socket, "send-keys", "-t", "t", "Enter")
        wait_for(
            socket,
            "Depends on",
            "dev-libs/a-1",
            "Needed by  0",
            "@selected  app-misc/world",
        )
        # Unfold dev-libs/a-1, the first link, in place.
        tmux(socket, "send-keys", "-t", "t", "Space")
        wait_for(socket, "╰─ dev-libs/alt-y-1", "├─ dev-libs/alt-x-1")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "1 of")
        # Clear the search, then only what depclean would remove.
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "/ to search")
        tmux(socket, "send-keys", "-t", "t", "o")
        wait_for(socket, " orphans", "app-misc/orphan-1", gone=["app-misc/world-1"])
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_tui_lists_and_shows_pending_updates(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("repository"), path)
    skip_without_tui()

    socket = f"egraph-test-{os.getpid()}"
    command = f"{EGRAPH} --store {path} --no-refresh tui; echo EXIT=$?; sleep 30"
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "110", "-y", "30", command)
    try:
        # It opens on the updates.
        wait_for(socket, " updates", "dev-libs/lib-2", gone=["app-misc/eula-1"])
        tmux(socket, "send-keys", "-t", "t", "/", "lib-2", "Enter")
        wait_for(socket, "1 updates")
        tmux(socket, "send-keys", "-t", "t", "Enter")
        wait_for(socket, "upgrade to dev-libs/lib-2.1  ::test_repo", "Update")
        # Sent apart, so that they are not read as one Alt key.
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "1 updates")
        tmux(socket, "send-keys", "-t", "t", "p")
        wait_for(socket, " merges", "dev-libs/lib  2 ", "2.1  ::test_repo")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_tui_searches_the_repositories(playgrounds, tmp_path):
    system = playgrounds("visibility")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    meta = egraph_store.RepositoryMeta("0", "0", "/", 0)
    index = repository.read(portdb(system))
    egraph_store.write(
        egraph_store.repository_path(path), egraph_store.encode_repository(index, meta)
    )
    skip_without_tui()

    socket = f"egraph-test-{os.getpid()}"
    command = f"{EGRAPH} --store {path} --no-refresh tui; echo EXIT=$?; sleep 30"
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "120", "-y", "30", command)
    try:
        wait_for(socket, "/ to search")
        tmux(socket, "send-keys", "-t", "t", "s", "testing", "Enter")
        wait_for(socket, "1 found", "app-misc/testing", "A toolkit still in testing")
        # Not installed: its versions and why each is masked.
        tmux(socket, "send-keys", "-t", "t", "Enter")
        wait_for(socket, "app-misc/testing  not installed", "masked: ~x86 keyword")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "1 found", gone=["not installed"])
        tmux(socket, "send-keys", "-t", "t", "/", *["BSpace"] * 7, "stable", "Enter")
        wait_for(socket, "app-misc/stable", gone=["app-misc/testing"])
        # Installed: its own page, with the versions in the repositories.
        tmux(socket, "send-keys", "-t", "t", "Enter")
        wait_for(socket, "Kept by", "Versions  2", "::test_repo", "::overlay")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_bare_egraph_opens_the_interface_and_runs_commands(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("repository"), path)
    skip_without_tui()

    socket = f"egraph-test-{os.getpid()}"
    command = f"{EGRAPH} --store {path} --no-refresh; echo EXIT=$?; sleep 30"
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "110", "-y", "30", command)
    try:
        wait_for(socket, "/ to search")
        tmux(socket, "send-keys", "-t", "t", ":", "updates -N", "Enter")
        # The output view's title and hints: the prompt shows the command while it runs.
        screen = wait_for(
            socket,
            ":updates -N  ",
            "esc back",
            "dev-libs/lib-2 ",
            "dev-libs/lib-2.1",
            "test_repo",
        )
        (row,) = [line for line in screen.splitlines() if "dev-libs/lib-2 " in line]
        assert row.split()[-4:] == [
            "dev-libs/lib-2",
            "upgrade",
            "dev-libs/lib-2.1",
            "test_repo",
        ]
        # The first row links to its package.
        tmux(socket, "send-keys", "-t", "t", "Enter")
        wait_for(socket, "Depends on")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, ":updates -N  ", gone=["Depends on"])
        tmux(socket, "send-keys", "-t", "t", ":", "nonsense", "Enter")
        wait_for(socket, "nonsense: no such command")
        tmux(socket, "send-keys", "-t", "t", "x", ":", "quit", "Enter")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_tui_previews_a_fresh_build_without_saving_it(system, tmp_path):
    if os.geteuid() == 0:
        pytest.skip("root rebuilds the store instead")
    skip_without_tui()
    playground, path, builder, _ = system
    assert query(system).returncode == 0
    before = path.read_bytes()
    add_package(playground, "dev-libs/alt-b-1")
    packages = len(installed.build(fresh_vardb(playground)).installed())

    socket = f"egraph-test-{os.getpid()}"
    command = (
        f"{EGRAPH} --store {path} --config-root {playground.eroot}"
        f" --eprefix {playground.eprefix} --builder {builder} --no-refresh tui;"
        " echo EXIT=$?; sleep 30"
    )
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "100", "-y", "16", command)
    try:
        wait_for(socket, " updates")
        # --no-refresh's warning would be lost under the interface, which repeats it.
        wait_for(socket, "answering from a stale store", " Warning ")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, gone=[" Warning "])
        tmux(socket, "send-keys", "-t", "t", "c")
        wait_for(
            socket,
            "differs from a fresh build",
            "dev-libs/alt-b-1",
            "u preview",
            seconds=60,
        )
        tmux(socket, "send-keys", "-t", "t", "u")
        wait_for(socket, "Showing the fresh build", "preview, not saved")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        tmux(socket, "send-keys", "-t", "t", "u")
        wait_for(socket, f"{packages} of {packages} packages")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")
    assert path.read_bytes() == before
    assert builds(system)[-1].startswith("--full --store ")
    assert str(path) not in builds(system)[-1]


def test_tui_refreshes_the_store_when_the_system_changes(system, tmp_path):
    skip_without_tui()
    playground, path, builder, _ = system
    assert query(system).returncode == 0
    packages = len(installed.build(fresh_vardb(playground)).installed())

    socket = f"egraph-test-{os.getpid()}"
    command = (
        f"{EGRAPH} --store {path} --config-root {playground.eroot}"
        f" --eprefix {playground.eprefix} --builder {builder} tui;"
        " echo EXIT=$?; sleep 30"
    )
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "100", "-y", "16", command)
    try:
        wait_for(socket, " updates")
        tmux(socket, "send-keys", "-t", "t", "u")
        wait_for(socket, f"{packages} of {packages} packages")
        add_package(playground, "dev-libs/alt-b-1")
        # No key pressed: the interface notices on its own.
        wait_for(socket, f"{packages + 1} of {packages + 1} packages", seconds=60)
        assert builds(system)[-1].startswith("--incremental --store ")
        # Commands answer from the refreshed store too.
        tmux(socket, "send-keys", "-t", "t", ":", "orphans", "Enter")
        wait_for(socket, ":orphans  ", "esc back", "dev-libs/alt-b-1")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, gone=[":orphans  "])
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_tui_builds_the_repository_index_the_first_time_it_searches(system, tmp_path):
    skip_without_tui()
    playground, path, builder, log = system
    assert query(system).returncode == 0
    # The playground's repositories, which only its environment names.
    repositories = playground.settings.environ()["PORTAGE_REPOSITORIES"]

    socket = f"egraph-test-{os.getpid()}"
    command = (
        f"env PORTAGE_REPOSITORIES={shlex.quote(repositories)}"
        f" {EGRAPH} --store {path} --config-root {playground.eroot}"
        f" --eprefix {playground.eprefix} --builder {builder} tui;"
        " echo EXIT=$?; sleep 30"
    )
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "120", "-y", "20", command)
    try:
        wait_for(socket, " updates")
        tmux(socket, "send-keys", "-t", "t", "s", "alt-a", "Enter")
        wait_for(socket, "1 found", "dev-libs/alt-a", seconds=120)
        # A command's search answers from the same index, built once.
        tmux(socket, "send-keys", "-t", "t", ":", "search alt-a", "Enter")
        wait_for(socket, ":search alt-a  ", "esc back", "dev-libs/alt-a")
        repository_builds = [
            line
            for line in log.read_text().splitlines()
            if line.startswith("--repository")
        ]
        assert len(repository_builds) == 1
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "1 found", gone=[":search alt-a  "])
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_tui_dismisses_and_puts_off_notices(updates_system, tmp_path):
    skip_without_tui()
    playground, path, builder, _ = updates_system
    assert run_egraph(updates_system, "status", "--update").returncode == 0
    status = json.loads((path.parent / "status.json").read_text())
    (path.parent / "notices.json").write_text(
        json.dumps(
            {
                "format": 1,
                "written": 1790000000,
                "stores": status["stores"],
                "repositories": [{"name": "test_repo", "synced": 1700000000}],
                "notices": [
                    {
                        "kind": "masked",
                        "key": "masked:app-misc/x-1",
                        "title": "app-misc/x-1 is masked",
                        "detail": ["by package.mask"],
                        "fingerprint": "package.mask",
                        "since": 1790000000,
                    }
                ],
            }
        )
    )
    state = path.parent / "state"

    socket = f"egraph-test-{os.getpid()}"
    command = (
        f"env XDG_STATE_HOME={state} {EGRAPH} --store {path}"
        f" --config-root {playground.eroot} --eprefix {playground.eprefix}"
        f" --builder {builder} --no-refresh tui; echo EXIT=$?; sleep 30"
    )
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "120", "-y", "20", command)
    try:
        wait_for(socket, " @installed  @world  @system  notices 2 ")
        tmux(socket, "send-keys", "-t", "t", "l", "l", "l")
        wait_for(
            socket, "2 notices", "masked    app-misc/x-1 is masked", "by package.mask"
        )
        tmux(socket, "send-keys", "-t", "t", "x")
        wait_for(socket, "1 notice  1 set aside", gone=["app-misc/x-1 is masked"])
        tmux(socket, "send-keys", "-t", "t", "z")
        wait_for(socket, " Put off ", "2  a day")
        tmux(socket, "send-keys", "-t", "t", "2")
        wait_for(socket, "0 notices  2 set aside", "Nothing needs you")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")
    set_aside = json.loads((state / "egraph" / "set-aside.json").read_text())[
        "set_aside"
    ]
    assert [(entry["key"], "until" in entry) for entry in set_aside] == [
        ("masked:app-misc/x-1", False),
        ("stale:test_repo", True),
    ]
    # The command line leaves out what the interface set aside.
    rows = run_egraph(updates_system, "status").stdout.splitlines()
    assert "notices\t0" in rows


def test_tui_watches_running_emerges(playgrounds, tmp_path):
    skip_without_tui()
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("roots"), path)
    # A live pid, as emerge's own would be.
    pid = os.getpid()
    run_dir = tmp_path / "run" / "portage"
    run_dir.mkdir(parents=True)

    def publish(completed, phase):
        snapshot = {
            "type": "snapshot",
            "schema": 1,
            "emerge_pid": pid,
            "timestamp": time.time(),
            "jobs": {"running": 1, "max": 2, "completed": completed, "total": 4},
            "tasks": [
                {
                    "cpv": "dev-libs/a-2",
                    "kind": "build",
                    "phase": phase,
                    "binary": False,
                    "merge_wait": False,
                    "elapsed": 3.0,
                    "build_elapsed": 3.0,
                }
            ],
        }
        (run_dir / f"emerge-{pid}.json").write_text(json.dumps(snapshot))

    publish(1, "compile")
    socket = f"egraph-test-{os.getpid()}"
    command = (
        f"{EGRAPH} --store {path} --eprefix {tmp_path} --no-refresh tui;"
        " echo EXIT=$?; sleep 30"
    )
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "120", "-y", "16", command)
    try:
        wait_for(socket, "/ to search")
        tmux(socket, "send-keys", "-t", "t", "e")
        wait_for(socket, f"emerge {pid}", "1 of 4 done", "dev-libs/a-2", "compile")
        # Read again on its own, with no key pressed.
        publish(3, "install")
        wait_for(socket, "3 of 4 done", "install")
        # The installed version's page.
        tmux(socket, "send-keys", "-t", "t", "Enter")
        wait_for(socket, "Depends on")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")


def test_tui_shows_the_merge_list_as_a_tree(gnupg_home, tmp_path):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    skip_without_tui()
    playground = ResolverPlayground(
        ebuilds={
            "dev-libs/lib-1": {"EAPI": "8"},
            "app-misc/app-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib"},
            "app-misc/plugin-1": {"EAPI": "8", "DEPEND": "app-misc/app"},
        },
        installed={"app-misc/app-0": {"EAPI": "8", "KEYWORDS": "x86"}},
    )
    try:
        vardb = playground.trees[playground.eroot]["vartree"].dbapi
        path = tmp_path / "installed.egraph"
        write_stores(System(playground.eroot, vardb, playground.trees), path)
        eprefix = Path(playground.eprefix)
        # emerge's merge list, and it running lib; its pid is a live one.
        edb = eprefix / "var" / "cache" / "edb"
        edb.mkdir(parents=True, exist_ok=True)
        mergelist = [
            ["ebuild", "/", cpv, "merge"]
            for cpv in ("dev-libs/lib-1", "app-misc/app-1", "app-misc/plugin-1")
        ]
        (edb / "mtimedb").write_text(json.dumps({"resume": {"mergelist": mergelist}}))
        pid = os.getpid()
        run_dir = eprefix / "run" / "portage"
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / f"emerge-{pid}.json").write_text(
            json.dumps(
                {
                    "type": "snapshot",
                    "schema": 1,
                    "emerge_pid": pid,
                    "jobs": {"running": 1, "max": 2, "completed": 0, "total": 3},
                    "tasks": [
                        {"cpv": "dev-libs/lib-1", "kind": "build", "phase": "compile"}
                    ],
                }
            )
        )
        socket = f"egraph-test-{os.getpid()}"
        command = (
            f"{EGRAPH} --store {path} --config-root {playground.eroot}"
            f" --eprefix {playground.eprefix} --no-refresh tui; echo EXIT=$?; sleep 30"
        )
        tmux(socket, "new-session", "-d", "-s", "t", "-x", "120", "-y", "20", command)
        try:
            wait_for(socket, "/ to search")
            tmux(socket, "send-keys", "-t", "t", "e")
            screen = wait_for(
                socket,
                "waits for 1",
                "dev-libs/lib-1",
                "compile",
                "app-misc/app-1",
                "app-misc/plugin-1",
            )
            lines = screen.splitlines()
            app = next(line for line in lines if "app-misc/app-1" in line)
            plugin = next(line for line in lines if "app-misc/plugin-1" in line)
            # app sits under lib, plugin under app.
            assert app.index("app-misc/app-1") < plugin.index("app-misc/plugin-1")
            # Down to app, whose installed version's page opens.
            tmux(socket, "send-keys", "-t", "t", "j")
            tmux(socket, "send-keys", "-t", "t", "Enter")
            wait_for(socket, "app-misc/app-0")
            tmux(socket, "send-keys", "-t", "t", "q")
            wait_for(socket, "EXIT=0")
        finally:
            tmux(socket, "kill-server")
    finally:
        playground.cleanup()


@pytest.fixture
def merges(gnupg_home, tmp_path):
    """A playground real merges happen in, with the worker's ebuilds."""
    from test_exec_run import over
    from test_worker import EBUILDS

    yield from over(EBUILDS, tmp_path)


def test_tui_removes_installs_and_shows_a_failed_build(merges, tmp_path):
    """Each action previewed, confirmed and run beside the interface, as egraph remove and
    egraph exec would run it; a failed build shows the end of its log."""
    skip_without_tui()
    system, machine = merges
    # depclean refuses to run with @world empty.
    machine.emerge("=app-misc/lib-1")
    machine.emerge("-1 =app-misc/files-1")
    age(system.playground.eroot)

    socket = f"egraph-test-{os.getpid()}"
    command = shlex.join(system.command("tui")) + "; echo EXIT=$?; sleep 30"
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "120", "-y", "30", command)
    try:
        wait_for(socket, " updates")
        tmux(socket, "send-keys", "-t", "t", "o")
        wait_for(socket, "1 orphans", "app-misc/files-1")
        tmux(socket, "send-keys", "-t", "t", "r")
        wait_for(socket, "Run egraph remove?", "1 package to remove", seconds=60)
        tmux(socket, "send-keys", "-t", "t", "y")
        wait_for(socket, "egraph remove finished", seconds=120)
        assert not machine.installed("app-misc/files-1")

        # The dialog, then the emerge view the run showed in.
        tmux(socket, "send-keys", "-t", "t", "Escape")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        tmux(socket, "send-keys", "-t", "t", "s", "needs", "Enter")
        wait_for(socket, "1 found", "app-misc/needs", seconds=120)
        tmux(socket, "send-keys", "-t", "t", "i")
        wait_for(socket, "Run egraph exec?", "app-misc/needs", seconds=60)
        tmux(socket, "send-keys", "-t", "t", "y")
        wait_for(socket, "egraph exec finished", seconds=120)
        assert machine.installed("app-misc/needs-1")

        tmux(socket, "send-keys", "-t", "t", "Escape")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        tmux(socket, "send-keys", "-t", "t", "s", "broken", "Enter")
        wait_for(socket, "1 found", "app-misc/broken")
        tmux(socket, "send-keys", "-t", "t", "i")
        wait_for(socket, "Run egraph exec?", seconds=60)
        tmux(socket, "send-keys", "-t", "t", "y")
        wait_for(
            socket,
            "egraph exec failed",
            "app-misc/broken-1 failed; its log",
            "cannot compile",
            seconds=120,
        )
        assert not machine.installed("app-misc/broken-1")
        tmux(socket, "send-keys", "-t", "t", "Escape")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")
