"""egraph notify against a fake notification server on a private session bus (test_bus.py's),
with the notices written as egraph watch writes them."""

import json
import os
import shutil
import signal
import subprocess
import time

from test_bus import Talker, bus, server, skip_without_bus  # noqa: F401 (fixtures)
from test_refresh import egraph as run_egraph
from test_refresh import updates_system  # noqa: F401 (a fixture)

EGRAPH = os.environ.get("EGRAPH")


def notice(kind, key, title):
    return {
        "kind": kind,
        "key": key,
        "title": title,
        "detail": [],
        "fingerprint": "1",
        "since": 1790000000,
    }


def write_notices(store, notices):
    """As egraph watch writes them: whole, by a rename."""
    status = json.loads((store.parent / "status.json").read_text())
    new = store.parent / "notices.json.new"
    new.write_text(
        json.dumps(
            {
                "format": 1,
                "written": 1790000000,
                "stores": status["stores"],
                "repositories": [],
                "notices": notices,
            }
        )
    )
    new.rename(store.parent / "notices.json")


def start(system, env, binary=EGRAPH):
    playground, store, builder, _ = system
    return subprocess.Popen(
        [
            str(binary),
            "--store",
            str(store),
            "--config-root",
            playground.eroot,
            "--eprefix",
            playground.eprefix,
            "--builder",
            str(builder),
            "notify",
        ],
        stderr=subprocess.PIPE,
        text=True,
        env=dict(
            playground.settings.environ(),
            DBUS_SESSION_BUS_ADDRESS=env["DBUS_SESSION_BUS_ADDRESS"],
            XDG_STATE_HOME=str(store.parent / "state"),
        ),
    )


def stop(daemon):
    daemon.send_signal(signal.SIGTERM)
    _, err = daemon.communicate(timeout=10)
    assert daemon.returncode == 0, err
    return err


def eventually(check, seconds=10):
    deadline = time.monotonic() + seconds
    while not check():
        assert time.monotonic() < deadline
        time.sleep(0.05)


def test_notify_sums_up_the_notices_and_does_what_its_buttons_say(
    updates_system, bus, server, tmp_path
):
    skip_without_bus()
    playground, store, _, _ = updates_system
    assert run_egraph(updates_system, "status", "--update").returncode == 0
    opened = tmp_path / "opened"
    terminal = tmp_path / "terminal"
    terminal.write_text(f'#!/bin/sh\necho "$@" > {opened}\n')
    terminal.chmod(0o755)
    settings = os.path.join(playground.eroot, "etc/egraph/egraph.conf")
    os.makedirs(os.path.dirname(settings), exist_ok=True)
    with open(settings, "w") as f:
        f.write(f"terminal = {terminal}\n")
    write_notices(
        store,
        [
            notice("masked", "masked:app-misc/x-1", "app-misc/x-1 is masked"),
            notice("glsa", "glsa:202601-01", "x: several flaws"),
        ],
    )
    state = store.parent / "state" / "egraph"

    daemon = start(updates_system, bus)
    try:
        posted = server.read()
        assert (posted["call"], posted["replaces"], posted["summary"]) == (
            "Notify",
            0,
            "2 notices",
        )
        assert posted["body"] == "app-misc/x-1 is masked\nx: several flaws"
        assert posted["actions"] == [
            "default",
            "Open",
            "open",
            "Open",
            "later",
            "Later",
            "dismiss",
            "Dismiss",
        ]
        # One a session.
        second = start(updates_system, bus)
        _, err = second.communicate(timeout=10)
        assert second.returncode == 1
        assert "another egraph notify is running" in err

        server.send({"action": [posted["id"], "dismiss"]})
        server.read()
        assert server.read() == {"call": "CloseNotification", "id": posted["id"]}
        eventually(lambda: (state / "set-aside.json").exists())
        set_aside = json.loads((state / "set-aside.json").read_text())["set_aside"]
        assert sorted((each["key"], "until" in each) for each in set_aside) == [
            ("glsa:202601-01", False),
            ("masked:app-misc/x-1", False),
        ]

        write_notices(
            store,
            [
                notice("masked", "masked:app-misc/x-1", "app-misc/x-1 is masked"),
                notice("glsa", "glsa:202601-01", "x: several flaws"),
                notice("glsa", "glsa:202601-02", "y: a flaw"),
            ],
        )
        posted = server.read()
        assert (posted["replaces"], posted["summary"]) == (0, "y: a flaw")
        server.send({"action": [posted["id"], "open"]})
        server.read()
        assert server.read() == {"call": "CloseNotification", "id": posted["id"]}
        eventually(lambda: opened.exists())
        words = opened.read_text().split()
        assert words[-2:] == ["tui", "--notices"]
        assert words[words.index("--store") + 1] == str(store)
    finally:
        err = stop(daemon)
    assert err == ""

    # What the last summary carried is not repeated by the next egraph notify.
    daemon = start(updates_system, bus)
    try:
        write_notices(
            store,
            [
                notice("glsa", "glsa:202601-02", "y: a flaw"),
                notice("glsa", "glsa:202601-03", "z: a flaw"),
            ],
        )
        posted = server.read()
        assert posted["summary"] == "2 notices, 1 new"
        assert posted["body"] == "z: a flaw\ny: a flaw"
    finally:
        stop(daemon)
    # Its summary goes with it.
    assert server.read() == {"call": "CloseNotification", "id": posted["id"]}


def test_notify_restarts_itself_when_its_executable_is_replaced(
    updates_system, bus, server, tmp_path
):
    """Its summary is taken down, and the new image, in the same process, posts it again."""
    skip_without_bus()
    store = updates_system[1]
    assert run_egraph(updates_system, "status", "--update").returncode == 0
    write_notices(store, [notice("glsa", "glsa:202601-01", "x: several flaws")])
    binary = tmp_path / "bin" / "egraph"
    binary.parent.mkdir()
    shutil.copy2(EGRAPH, binary)
    daemon = start(updates_system, bus, binary)
    try:
        posted = server.read()
        assert posted["summary"] == "x: several flaws"
        shutil.copy2(EGRAPH, binary.with_name("egraph.new"))
        os.rename(binary.with_name("egraph.new"), binary)
        assert server.read() == {"call": "CloseNotification", "id": posted["id"]}
        again = server.read()
        assert (again["call"], again["replaces"], again["summary"]) == (
            "Notify",
            0,
            "x: several flaws",
        )
        assert daemon.poll() is None
        assert os.stat(f"/proc/{daemon.pid}/exe").st_ino == binary.stat().st_ino
    finally:
        err = stop(daemon)
    assert err == "egraph: notify: restarting, as its executable was replaced\n"
