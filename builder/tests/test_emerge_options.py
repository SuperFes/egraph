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


def run(system, output, monkeypatch, environment=None, **variables):
    monkeypatch.delenv("EMERGE_DEFAULT_OPTS", raising=False)
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    if environment is not None:
        monkeypatch.setenv("EMERGE_DEFAULT_OPTS", environment)
    for name, value in variables.items():
        monkeypatch.setenv(name, value)
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
    assert written["jobserver"] is None
    assert written["tmpdir"] == os.path.join(system.eprefix, "var/tmp")


def test_the_environment_wins_as_in_emerge(system, tmp_path, monkeypatch):
    output = tmp_path / "options"
    assert run(system, output, monkeypatch, "-j2") == cli.EXIT_OK
    assert json.loads(output.read_text())["options"] == ["-j2"]


def test_the_jobserver_is_written_as_emerge_finds_it(system, tmp_path, monkeypatch):
    output = tmp_path / "options"
    flags = "-j4 --jobserver-auth=fifo:/run/jobserver"
    assert (
        run(system, output, monkeypatch, FEATURES="jobserver-token", MAKEFLAGS=flags)
        == cli.EXIT_OK
    )
    assert json.loads(output.read_text())["jobserver"] == "/run/jobserver"


@pytest.mark.parametrize(
    "features, makeflags, expected",
    [
        ("", "--jobserver-auth=fifo:/js", None),
        ("jobserver-token", None, None),
        ("jobserver-token", "-j4 --jobserver-auth=fifo:/js", "/js"),
        # Only a named pipe, and the last one given.
        ("jobserver-token", "--jobserver-auth=3,4", None),
        ("jobserver-token", "--jobserver-auth=fifo:/a --jobserver-auth=fifo:/b", "/b"),
        ("jobserver-token", "--jobserver-auth=fifo:/a --jobserver-auth=3,4", None),
    ],
)
def test_the_jobserver_is_the_last_named_pipe_in_makeflags(
    features, makeflags, expected
):
    values = {"FEATURES": features}
    if makeflags is not None:
        values["MAKEFLAGS"] = makeflags
    assert cli.jobserver(Settings(**values)) == expected


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
