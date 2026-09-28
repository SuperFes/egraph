"""egraph tui in a real terminal: a private tmux server stands in for one."""

import os
import shutil
import subprocess
import time

import pytest

from egraph_build import installed, store
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
    store.write(
        path, store.encode(installed.build(vardb), store.Meta("0", "0", "/", 0))
    )
    skip_without_tui()

    socket = f"egraph-test-{os.getpid()}"
    command = f"{EGRAPH} --store {path} --no-refresh tui; echo EXIT=$?; sleep 30"
    tmux(socket, "new-session", "-d", "-s", "t", "-x", "90", "-y", "16", command)
    try:
        packages = len(installed.build(vardb).installed())
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
        wait_for(socket, f"{packages - 1} of {packages - 1} packages")
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
        wait_for(socket, f"{packages} of {packages} packages")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")
    assert path.read_bytes() == before
    assert builds(system)[-1].startswith("--full --store ")
    assert str(path) not in builds(system)[-1]
