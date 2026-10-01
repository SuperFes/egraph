"""egraph-build --emerge-options: EMERGE_DEFAULT_OPTS as emerge splits it, and where the elog
summary goes."""

import json
import os

import pytest

from egraph_build import cli


@pytest.fixture(scope="module")
def system(gnupg_home):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(
        user_config={
            "make.conf": (
                "EMERGE_DEFAULT_OPTS=\"--jobs 4 --keep-going '--exclude=a/b'\"",
            )
        }
    )
    yield playground
    playground.cleanup()


def run(system, output, monkeypatch, environment=None):
    monkeypatch.delenv("EMERGE_DEFAULT_OPTS", raising=False)
    if environment is not None:
        monkeypatch.setenv("EMERGE_DEFAULT_OPTS", environment)
    argv = ["--emerge-options", "--output", str(output)]
    argv += ["--config-root", system.eroot, "--eprefix", system.eprefix]
    return cli.main(argv)


def test_make_conf_options_are_written_as_emerge_splits_them(
    system, tmp_path, monkeypatch
):
    output = tmp_path / "options"
    assert run(system, output, monkeypatch) == cli.EXIT_OK
    written = json.loads(output.read_text())
    assert written["options"] == ["--jobs", "4", "--keep-going", "--exclude=a/b"]
    # make.globals: save_summary and echo, the summary holding every class echo shows.
    assert written["elog"] == {
        "summary": os.path.join(system.eprefix, "var/log/portage/elog/summary.log"),
        "system": "save_summary:log,warn,error,qa",
    }


def test_the_environment_wins_as_in_emerge(system, tmp_path, monkeypatch):
    output = tmp_path / "options"
    assert run(system, output, monkeypatch, "-j2") == cli.EXIT_OK
    assert json.loads(output.read_text())["options"] == ["-j2"]


def test_it_needs_an_output(capsys):
    assert cli.main(["--emerge-options"]) == cli.EXIT_USAGE


class Settings(dict):
    def __init__(self, **values):
        super().__init__(BROOT="/", PORTAGE_ELOG_CLASSES="log warn error", **values)


@pytest.mark.parametrize(
    "system, expected",
    [
        # Without save_summary, nothing to read: emerge's echo stays.
        ("echo", (None, None)),
        ("save_summary", ("/var/log/portage/elog/summary.log", None)),
        ("save-summary echo", ("/var/log/portage/elog/summary.log", "save-summary")),
        ("echo:warn save_summary:error", ("/var/log/portage/elog/summary.log", None)),
        (
            "save_summary:error echo:error mail",
            ("/var/log/portage/elog/summary.log", "save_summary:error mail"),
        ),
        # The same module twice adds its classes up.
        (
            "save_summary:log save_summary:warn,error echo",
            (
                "/var/log/portage/elog/summary.log",
                "save_summary:log save_summary:warn,error",
            ),
        ),
    ],
)
def test_echo_goes_only_where_the_summary_holds_all_it_shows(system, expected):
    from egraph_build import notices

    assert notices.elog_settings(Settings(PORTAGE_ELOG_SYSTEM=system)) == expected


def test_the_summary_follows_portage_logdir():
    from egraph_build import notices

    settings = Settings(
        PORTAGE_ELOG_SYSTEM="save_summary", PORTAGE_LOGDIR="/logs//here"
    )
    assert notices.elog_settings(settings)[0] == "/logs/here/elog/summary.log"
