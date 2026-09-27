import re
from pathlib import Path

import pytest

from egraph_build import cli, installed


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


@pytest.mark.parametrize("mode", ["full", "incremental"])
def test_unimplemented_modes_say_so(mode, capsys):
    assert cli.main([f"--{mode}"]) == cli.EXIT_NOT_IMPLEMENTED
    assert capsys.readouterr().err == f"egraph-build: {mode}: not implemented\n"


def test_roots_default_to_environment(monkeypatch):
    monkeypatch.setenv("ROOT", "/mnt/target")
    monkeypatch.setenv("PORTAGE_CONFIGROOT", "/mnt/config")
    args = cli.parser().parse_args([])
    assert (args.root, args.config_root) == ("/mnt/target", "/mnt/config")


def test_json_prints_the_installed_layer(monkeypatch, capsys, playgrounds):
    vardb = playgrounds("reference").vardb
    opened = []

    def open_vardb(config_root, root):
        opened.append((config_root, root))
        return vardb

    monkeypatch.setattr(cli, "open_vardb", open_vardb)
    assert cli.main(["--json", "--root", "/r", "--config-root", "/c"]) == cli.EXIT_OK
    assert opened == [("/c", "/r")]
    assert capsys.readouterr().out == installed.to_json(installed.build(vardb))


def test_exit_codes_match_egraph():
    header = Path(__file__).parents[2] / "src" / "cli.hpp"
    enum = re.search(r"enum class Exit[^{]*\{([^}]*)\}", header.read_text()).group(1)
    egraph = {name: int(value) for name, value in re.findall(r"(\w+) = (\d+)", enum)}
    builder = {
        name[len("EXIT_") :].lower(): value
        for name, value in vars(cli).items()
        if name.startswith("EXIT_")
    }
    assert egraph and builder == egraph
