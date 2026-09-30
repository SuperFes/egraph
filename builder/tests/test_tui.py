"""egraph tui in a real terminal: a private tmux server stands in for one."""

import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

from conftest import System, write_stores
from egraph_build import installed
from test_build import add_package, fresh_vardb
from test_refresh import builds, query, system  # noqa: F401 (a fixture)

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


def wait_for(socket, text, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        screen = tmux(socket, "capture-pane", "-p", "-t", "t").stdout
        if text in screen:
            return screen
        time.sleep(0.1)
    raise AssertionError(f"{text!r} never appeared:\n{screen}")


def wait_gone(socket, text, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        screen = tmux(socket, "capture-pane", "-p", "-t", "t").stdout
        if text not in screen:
            return screen
        time.sleep(0.1)
    raise AssertionError(f"{text!r} never went away:\n{screen}")


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
        screen = wait_for(socket, "Depends on")
        assert "dev-libs/a-1" in screen
        assert "Needed by  0" in screen
        assert "@selected  app-misc/world" in screen
        # Unfold dev-libs/a-1, the first link, in place.
        tmux(socket, "send-keys", "-t", "t", "Space")
        screen = wait_for(socket, "╰─ dev-libs/alt-y-1")
        assert "├─ dev-libs/alt-x-1" in screen
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "1 of")
        # Clear the search, then only what depclean would remove.
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "/ to search")
        tmux(socket, "send-keys", "-t", "t", "o")
        screen = wait_for(socket, " orphans")
        assert "app-misc/orphan-1" in screen
        assert "app-misc/world-1" not in screen
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
        screen = wait_for(socket, " updates")
        assert "dev-libs/lib-2" in screen
        assert "app-misc/eula-1" not in screen
        tmux(socket, "send-keys", "-t", "t", "/", "lib-2", "Enter")
        wait_for(socket, "1 updates")
        tmux(socket, "send-keys", "-t", "t", "Enter")
        screen = wait_for(socket, "upgrade to dev-libs/lib-2.1  ::test_repo")
        assert "Update" in screen
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
        # The output view's own hints: the prompt shows the command while it runs.
        screen = wait_for(socket, "command  esc back")
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
        wait_for(socket, "command  esc back")
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
        screen = wait_for(socket, "answering from a stale store")
        assert " Warning " in screen
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_gone(socket, " Warning ")
        tmux(socket, "send-keys", "-t", "t", "c")
        screen = wait_for(socket, "differs from a fresh build", seconds=60)
        assert "dev-libs/alt-b-1" in screen
        assert "u preview" in screen
        tmux(socket, "send-keys", "-t", "t", "u")
        screen = wait_for(socket, "Showing the fresh build")
        assert "preview, not saved" in screen
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
        screen = wait_for(socket, f"emerge {pid}")
        assert "1 of 4 done" in screen
        assert "dev-libs/a-2" in screen
        assert "compile" in screen
        # Read again on its own, with no key pressed.
        publish(3, "install")
        screen = wait_for(socket, "3 of 4 done")
        assert "install" in screen
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
            screen = wait_for(socket, "waits for 1", seconds=30)
            assert "dev-libs/lib-1" in screen
            assert "compile" in screen
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
