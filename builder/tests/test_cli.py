import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from egraph_build import cli, evaluated, installed


@pytest.mark.parametrize(
    "argv, mode",
    [
        ([], "full"),
        (["--full"], "full"),
        (["--incremental"], "incremental"),
        (["--json"], "json"),
    ],
)
def test_modes(argv, mode):
    assert cli.parser().parse_args(argv).mode == mode


def test_store_defaults_to_environment(monkeypatch):
    monkeypatch.setenv("EGRAPH_STORE", "/tmp/x.egraph")
    assert str(cli.parser().parse_args([]).store) == "/tmp/x.egraph"
    assert str(cli.parser().parse_args(["--store", "y"]).store) == "y"


def test_modes_are_exclusive(capsys):
    assert cli.main(["--full", "--incremental"]) == cli.EXIT_USAGE


def test_help_exits_cleanly(capsys):
    assert cli.main(["--help"]) == cli.EXIT_OK


def test_roots_default_to_portages_environment(monkeypatch):
    monkeypatch.setenv("ROOT", "/mnt/target")
    monkeypatch.setenv("PORTAGE_CONFIGROOT", "/mnt/config")
    monkeypatch.setenv("PORTAGE_OVERRIDE_EPREFIX", "/prefix")
    args = cli.parser().parse_args([])
    assert (args.root, args.config_root, args.eprefix) == (
        "/mnt/target",
        "/mnt/config",
        "/prefix",
    )


def test_unset_roots_are_left_to_portage(monkeypatch):
    for name in ("ROOT", "PORTAGE_CONFIGROOT", "PORTAGE_OVERRIDE_EPREFIX"):
        monkeypatch.delenv(name, raising=False)
    args = cli.parser().parse_args([])
    assert (args.root, args.config_root, args.eprefix) == (None, None, None)


def test_json_prints_the_installed_layer(monkeypatch, capsys, playgrounds):
    vardb = playgrounds("reference").vardb
    opened = []

    def open_vardb(config_root, root, eprefix):
        opened.append((config_root, root, eprefix))
        return vardb

    monkeypatch.setattr(cli, "open_vardb", open_vardb)
    argv = ["--json", "--root", "/r", "--config-root", "/c", "--eprefix", "/p"]
    assert cli.main(argv) == cli.EXIT_OK
    assert opened == [("/c", "/r", "/p")]
    assert capsys.readouterr().out == installed.to_json(installed.build(vardb))


def test_evaluated_json_prints_the_evaluated_layer(monkeypatch, capsys, playgrounds):
    system = playgrounds("repository")
    portdb = system.trees[system.eroot]["porttree"].dbapi
    monkeypatch.setattr(cli, "open_databases", lambda *args: (system.vardb, portdb))
    assert cli.main(["--evaluated-json"]) == cli.EXIT_OK
    expected = evaluated.to_json(evaluated.build(system.vardb, portdb))
    assert capsys.readouterr().out == expected


def test_exit_codes_match_egraph():
    header = Path(__file__).parents[2] / "src" / "cli.hpp"
    enum = re.search(r"enum class Exit[^{]*\{([^}]*)\}", header.read_text()).group(1)
    egraph = {name: int(value) for name, value in re.findall(r"(\w+) = (\d+)", enum)}
    # Only the interface's previews stop there; egraph never exits with it.
    del egraph["previewed"]
    builder = {
        name[len("EXIT_") :].lower(): value
        for name, value in vars(cli).items()
        if name.startswith("EXIT_")
    }
    assert egraph and builder == egraph


def test_the_environment_does_not_override_the_configuration(playgrounds, monkeypatch):
    """A hook portage runs inherits its configuration, USE and the USE_EXPAND variables among
    it; taken back from there, they would outrank package.use."""
    system = playgrounds("atoms")
    settings = system.vardb.settings
    monkeypatch.setenv("PORTAGE_REPOSITORIES", settings["PORTAGE_REPOSITORIES"])
    monkeypatch.setenv("USE", "-a -b")
    monkeypatch.setenv("KERNEL", "hurd")
    from portage import config

    where = (settings["PORTAGE_CONFIGROOT"], settings["ROOT"], settings["EPREFIX"])
    _, portdb = cli.open_databases(*where)
    clone = config(clone=portdb.settings)
    clone.setcpv("app-misc/u8-1", mydb=portdb)
    enabled = clone["PORTAGE_USE"].split()
    assert {"a", "b"} <= set(enabled)
    assert "kernel_hurd" not in enabled
    env = cli.portage_environment(*where)
    assert "USE" not in env and "KERNEL" not in env


def test_the_installed_script_runs_nothing_when_multiprocessing_reruns_it(tmp_path):
    # portage's fetch spawns its helpers with the spawn start method under Python 3.14, which runs
    # the parent's script again as __mp_main__ (multiprocessing.spawn._fixup_main_from_path).
    script = tmp_path / "egraph-build"
    template = Path(__file__).parents[1] / "egraph-build.py.in"
    script.write_text(template.read_text().replace("@PYTHON@", sys.executable))
    env = dict(os.environ, PYTHONPATH=os.pathsep.join(sys.path))

    def run(name):
        code = (
            "import runpy, sys; sys.argv = [sys.argv[1], '--help']; "
            f"runpy.run_path(sys.argv[0], run_name={name!r})"
        )
        return subprocess.run(
            [sys.executable, "-c", code, str(script)],
            env=env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
        )

    rerun = run("__mp_main__")
    assert (rerun.returncode, rerun.stdout, rerun.stderr) == (0, "", "")
    assert "usage: egraph-build" in run("__main__").stdout
