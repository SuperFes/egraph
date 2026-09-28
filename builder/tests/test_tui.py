"""egraph tui in a real terminal: a private tmux server stands in for one."""

import os
import shutil
import subprocess
import time

import pytest

from egraph_build import installed, store

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


def test_tui_shows_the_store_and_quits(playgrounds, tmp_path):
    vardb = playgrounds("roots").vardb
    path = tmp_path / "installed.egraph"
    store.write(
        path, store.encode(installed.build(vardb), store.Meta("0", "0", "/", 0))
    )
    built = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "tui"],
        capture_output=True,
        text=True,
    )
    if built.returncode == 3:
        pytest.skip("egraph was built without the terminal interface")

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
        tmux(socket, "send-keys", "-t", "t", "Escape")
        wait_for(socket, "1 of")
        tmux(socket, "send-keys", "-t", "t", "q")
        wait_for(socket, "EXIT=0")
    finally:
        tmux(socket, "kill-server")
