"""egraph-build --emerge-options: EMERGE_DEFAULT_OPTS as emerge splits it."""

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


def test_make_conf_options_are_written_a_word_a_line(system, tmp_path, monkeypatch):
    output = tmp_path / "options"
    assert run(system, output, monkeypatch) == cli.EXIT_OK
    assert output.read_text() == "--jobs\n4\n--keep-going\n--exclude=a/b\n"


def test_the_environment_wins_as_in_emerge(system, tmp_path, monkeypatch):
    output = tmp_path / "options"
    assert run(system, output, monkeypatch, "-j2") == cli.EXIT_OK
    assert output.read_text() == "-j2\n"


def test_it_needs_an_output(capsys):
    assert cli.main(["--emerge-options"]) == cli.EXIT_USAGE
