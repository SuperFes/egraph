"""egraph's session bus layer (src/bus.cpp), through tests/bus_probe.cpp, against a fake
notification server (fake_notifications.py) on a private bus, so the desktop's own is never
touched."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

PROBE = os.environ.get("EGRAPH_BUS_PROBE")
SERVER = Path(__file__).with_name("fake_notifications.py")

# No service directories: nothing is ever activated, as a notification daemon would be.
CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={socket}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


def skip_without_bus():
    if not PROBE:
        pytest.skip("set EGRAPH_BUS_PROBE to bus-probe (built with the notify feature)")
    if not shutil.which("dbus-daemon"):
        pytest.skip("needs dbus-daemon")
    try:
        import dbus  # noqa: F401
        from gi.repository import GLib  # noqa: F401
    except ImportError:
        pytest.skip("needs dbus-python and PyGObject for the fake notification server")


@pytest.fixture
def bus(tmp_path):
    skip_without_bus()
    socket = tmp_path / "bus"
    config = tmp_path / "bus.conf"
    config.write_text(CONFIG.format(socket=socket))
    daemon = subprocess.Popen(
        ["dbus-daemon", "--nofork", "--print-address=1", f"--config-file={config}"],
        stdout=subprocess.PIPE,
        text=True,
    )
    address = daemon.stdout.readline().strip()
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
    try:
        yield env
    finally:
        daemon.terminate()
        daemon.wait(timeout=10)


class Talker:
    """A process answering a JSON line with a JSON line."""

    def __init__(self, argv, env):
        self.process = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            env=env,
        )

    def send(self, request):
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()

    def read(self):
        line = self.process.stdout.readline()
        assert line, "the process ended"
        return line if line == "ready\n" else json.loads(line)

    def ask(self, request):
        self.send(request)
        return self.read()

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)


@pytest.fixture
def server(bus):
    fake = Talker([sys.executable, str(SERVER)], bus)
    assert fake.read() == "ready\n"
    try:
        yield fake
    finally:
        fake.close()


@pytest.fixture
def probe(bus):
    talker = Talker([PROBE], bus)
    try:
        yield talker
    finally:
        talker.close()


ACTIONS = [
    ["default", "Open"],
    ["open", "Open"],
    ["later", "Later"],
    ["dismiss", "Dismiss"],
]


def test_posts_and_replaces_a_notification(server, probe):
    posted = probe.ask(
        {"notify": {"summary": "2 notices", "body": "one\ntwo", "actions": ACTIONS}}
    )
    assert posted == {"id": 1}
    assert server.read() == {
        "call": "Notify",
        "app_name": "egraph",
        "replaces": 0,
        "icon": "",
        "summary": "2 notices",
        "body": "one\ntwo",
        "actions": [word for pair in ACTIONS for word in pair],
        "hints": {},
        "expire": -1,
        "id": 1,
    }
    replaced = probe.ask(
        {"notify": {"replaces": 1, "summary": "3 notices", "expire": 0}}
    )
    assert replaced == {"id": 1}
    call = server.read()
    assert (call["replaces"], call["summary"], call["expire"]) == (1, "3 notices", 0)


def test_reports_actions_and_closing_of_its_own_notifications_only(server, probe):
    assert probe.ask({"notify": {"summary": "a notice", "actions": ACTIONS}}) == {
        "id": 1
    }
    server.read()
    # Another program's notification first.
    server.send({"action": [99, "open"]})
    server.read()
    server.send({"action": [1, "later"]})
    server.read()
    assert probe.ask({"wait": 5000}) == {
        "events": [{"kind": "action", "id": 1, "action": "later"}]
    }
    assert probe.ask({"close": 1}) == {}
    assert server.read() == {"call": "CloseNotification", "id": 1}
    assert probe.ask({"wait": 5000}) == {
        "events": [{"kind": "closed", "id": 1, "action": ""}]
    }
    # Closed: no longer its own.
    server.send({"action": [1, "open"]})
    server.read()
    assert probe.ask({"wait": 300}) == {"events": []}


def test_a_notification_closed_by_the_server_is_reported(server, probe):
    assert probe.ask({"notify": {"summary": "a notice"}}) == {"id": 1}
    server.read()
    # 1: expired.
    server.send({"closed": [1, 1]})
    server.read()
    assert probe.ask({"wait": 5000}) == {
        "events": [{"kind": "closed", "id": 1, "action": ""}]
    }


def test_a_name_is_claimed_by_one_connection_at_a_time(bus, probe):
    name = "io.github.SuperFes.egraph.Notify"
    assert probe.ask({"claim": name}) == {"claimed": True}
    other = Talker([PROBE], bus)
    try:
        assert other.ask({"claim": name}) == {"claimed": False}
    finally:
        other.close()
    # Its own already.
    assert probe.ask({"claim": name}) == {"claimed": True}


def test_without_a_notification_server_posting_says_so(probe):
    answer = probe.ask({"notify": {"summary": "a notice"}})
    assert "org.freedesktop.Notifications" in answer["error"]


def test_without_a_session_bus_opening_says_so(tmp_path):
    skip_without_bus()
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=f"unix:path={tmp_path / 'none'}")
    ran = subprocess.run([PROBE], input="", capture_output=True, text=True, env=env)
    assert ran.returncode == 1
    assert json.loads(ran.stdout)["error"].startswith(
        "cannot connect to the session bus: "
    )
