import pytest

from egraph_build import cli


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


@pytest.mark.parametrize("mode", ["full", "incremental", "json"])
def test_unimplemented_modes_say_so(mode, capsys):
    assert cli.main([f"--{mode}"]) == cli.EXIT_NOT_IMPLEMENTED
    assert capsys.readouterr().err == f"egraph-build: {mode}: not implemented\n"
