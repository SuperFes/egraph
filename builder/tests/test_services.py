"""The egraphd services meson installs: the systemd unit and the OpenRC script, which run
egraph watch as the egraph user."""

import configparser
import os
import shutil
import subprocess
from pathlib import Path

import pytest

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary")


def built(name):
    return Path(EGRAPH).parent / name


def unit():
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    parser.read_string(built("egraphd.service").read_text())
    return parser


def test_the_unit_runs_egraph_watch_as_the_egraph_user_with_only_its_directories_writable():
    service = unit()["Service"]
    assert service["ExecStart"].endswith("bin/egraph watch")
    assert service["User"] == "egraph"
    assert "portage" in service["SupplementaryGroups"].split()
    assert service["CacheDirectory"] == "egraph"
    assert service["StateDirectory"] == "egraph"
    assert service["ProtectSystem"] == "strict"


@pytest.mark.skipif(not shutil.which("systemd-analyze"), reason="needs systemd-analyze")
def test_the_unit_verifies(tmp_path):
    text = built("egraphd.service").read_text()
    command = unit()["Service"]["ExecStart"].split()[0]
    path = tmp_path / "egraphd.service"
    path.write_text(text.replace(command, EGRAPH))
    result = subprocess.run(
        ["systemd-analyze", "verify", "--man=no", str(path)],
        capture_output=True,
        text=True,
    )
    ours = [line for line in result.stderr.splitlines() if "egraphd" in line]
    assert result.returncode == 0 and not ours, result.stderr


def test_the_openrc_script_supervises_egraph_watch_as_the_egraph_user():
    script = built("egraphd")
    result = subprocess.run(
        [
            "sh",
            "-ec",
            'checkpath() { echo "checkpath $*"; }\n'
            '. "$1"\n'
            "start_pre\n"
            'echo "$supervisor $command $command_args $command_user"\n',
            "sh",
            str(script),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    checkpath, run = result.stdout.splitlines()
    assert checkpath.startswith(
        "checkpath --directory --owner egraph:egraph --mode 0755 "
    )
    assert checkpath.endswith(" /var/cache/egraph /var/lib/egraph")
    assert run.startswith("supervise-daemon ")
    assert run.endswith("bin/egraph watch egraph:egraph")
