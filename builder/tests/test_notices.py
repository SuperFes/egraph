"""egraph-build --notices: configuration updates waiting and unread news, as emerge sees them."""

import json
import os

import pytest

from egraph_build import cli, notices

NEWS = """Title: Something happened
Author: A Developer <dev@example.org>
Posted: 2026-09-01
Revision: 1
News-Item-Format: 2.0

Title: not a header
"""


def write(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


@pytest.fixture(scope="module")
def system(gnupg_home):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(
        ebuilds={"app-misc/a-1": {"EAPI": "8"}},
        user_config={"make.conf": ('CONFIG_PROTECT="/etc /opt/thing.conf"',)},
    )
    etc = os.path.join(playground.eroot, "etc")
    for name in (
        "foo.conf",
        "._cfg0000_foo.conf",
        "._cfg0001_foo.conf",
        "sub/._cfg0000_bar",
        # Backups and dot directories are not updates.
        "._cfg0000_old.bak",
        "._cfg0000_old~",
        ".git/._cfg0000_hidden",
    ):
        write(os.path.join(etc, name))
    opt = os.path.join(playground.eroot, "opt")
    write(os.path.join(opt, "thing.conf"))
    write(os.path.join(opt, "._cfg0000_thing.conf"))
    write(os.path.join(opt, "._cfg0000_other.conf"))
    repo = playground.settings.repositories["test_repo"].location
    for item in ("2026-09-01-something", "2026-09-02-other"):
        write(os.path.join(repo, "metadata", "news", item, f"{item}.en.txt"), NEWS)
    unread = os.path.join(playground.eroot, "var", "lib", "gentoo", "news")
    write(os.path.join(unread, "news-test_repo.unread"), "2026-09-01-something\n")
    yield playground
    playground.cleanup()


def test_config_updates_are_the_files_emerge_names(system):
    etc = os.path.join(system.eroot, "etc")
    opt = os.path.join(system.eroot, "opt")
    assert notices.config_updates(system.settings) == [
        (f"{etc}/foo.conf", f"{etc}/._cfg0000_foo.conf"),
        (f"{etc}/foo.conf", f"{etc}/._cfg0001_foo.conf"),
        (f"{etc}/sub/bar", f"{etc}/sub/._cfg0000_bar"),
        # A protected file: only its own updates.
        (f"{opt}/thing.conf", f"{opt}/._cfg0000_thing.conf"),
    ]


def test_unread_news_is_the_unread_list_with_titles(system):
    portdb = system.trees[system.eroot]["porttree"].dbapi
    assert notices.unread_news(system.settings, portdb) == [
        ("test_repo", "2026-09-01-something", "Something happened")
    ]


def test_no_news_without_the_news_feature(system):
    import portage

    settings = portage.config(clone=system.settings)
    settings.unlock()
    settings.features.discard("news")
    portdb = system.trees[system.eroot]["porttree"].dbapi
    assert notices.unread_news(settings, portdb) == []


def test_the_cli_writes_both_as_json(system, tmp_path):
    output = tmp_path / "notices"
    argv = ["--notices", "--output", str(output)]
    argv += ["--config-root", system.eroot, "--eprefix", system.eprefix]
    assert cli.main(argv) == cli.EXIT_OK
    written = json.loads(output.read_text())
    assert [update["file"] for update in written["config"]] == [
        f"{system.eroot}etc/foo.conf",
        f"{system.eroot}etc/foo.conf",
        f"{system.eroot}etc/sub/bar",
        f"{system.eroot}opt/thing.conf",
    ]
    assert written["news"] == [
        {
            "repo": "test_repo",
            "item": "2026-09-01-something",
            "title": "Something happened",
        }
    ]


def test_it_needs_an_output():
    assert cli.main(["--notices"]) == cli.EXIT_USAGE
