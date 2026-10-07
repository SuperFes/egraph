"""egraph-build --notices: configuration updates waiting and unread news, as emerge sees them."""

import json
import os

import pytest

from conftest import notice_lines, write_index, write_stores
from portage.dep.soname.parse import parse_soname_deps

import update
from egraph_build import cli, notices

EGRAPH = os.environ.get("EGRAPH")
needs_egraph = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

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


def item_text(system, item):
    repo = system.settings.repositories["test_repo"].location
    return os.path.join(repo, "metadata", "news", item, f"{item}.en.txt")


def test_unread_news_is_the_unread_list_with_titles(system):
    portdb = system.trees[system.eroot]["porttree"].dbapi
    assert notices.unread_news(system.settings, portdb) == [
        (
            "test_repo",
            "2026-09-01-something",
            "Something happened",
            item_text(system, "2026-09-01-something"),
        )
    ]


@pytest.fixture
def news_lists(system):
    """The playground's news lists, put back after the test."""
    directory = os.path.join(system.eroot, "var", "lib", "gentoo", "news")
    unread = os.path.join(directory, "news-test_repo.unread")
    read = os.path.join(directory, "news-test_repo.read")
    with open(unread) as f:
        kept = f.read()
    yield unread, read
    write(unread, kept)
    if os.path.exists(read):
        os.remove(read)


def test_reading_news_moves_it_to_the_read_list_as_eselect_does(system, news_lists):
    unread, read = news_lists
    write(unread, "2026-09-01-something\n2026-09-02-other\n")
    write(read, "2026-08-01-older\n")
    notices.mark_read(system.settings, "test_repo", "2026-09-02-other")
    with open(unread) as f:
        assert f.read() == "2026-09-01-something\n"
    with open(read) as f:
        assert f.read() == "2026-08-01-older\n2026-09-02-other\n"
    # Group-writable and world-readable, as GLEP 42 asks.
    assert os.stat(read).st_mode & 0o064 == 0o064
    # Read already: nothing changes.
    notices.mark_read(system.settings, "test_repo", "2026-09-02-other")
    with open(read) as f:
        assert f.read() == "2026-08-01-older\n2026-09-02-other\n"
    portdb = system.trees[system.eroot]["porttree"].dbapi
    assert [item for _, item, _, _ in notices.unread_news(system.settings, portdb)] == [
        "2026-09-01-something"
    ]


def test_the_cli_reads_news_items(system, news_lists):
    unread, read = news_lists
    roots = ["--config-root", system.eroot, "--eprefix", system.eprefix]
    argv = ["--news-read", "test_repo/2026-09-01-something", *roots]
    assert cli.main(argv) == cli.EXIT_OK
    with open(unread) as f:
        assert f.read() == ""
    with open(read) as f:
        assert f.read() == "2026-09-01-something\n"
    assert cli.main(["--news-read", *roots]) == cli.EXIT_USAGE
    assert cli.main(["--news-read", "no-slash", *roots]) == cli.EXIT_USAGE


@pytest.mark.skipif(os.geteuid() == 0, reason="root writes anything")
def test_news_that_cannot_be_marked_read_says_why(system, news_lists, capsys):
    directory = os.path.dirname(news_lists[0])
    os.chmod(directory, 0o555)
    try:
        argv = ["--news-read", "test_repo/2026-09-01-something"]
        argv += ["--config-root", system.eroot, "--eprefix", system.eprefix]
        assert cli.main(argv) == cli.EXIT_FAILURE
    finally:
        os.chmod(directory, 0o755)
    assert "egraph-build: cannot mark test_repo/2026-09-01-something read: " in (
        capsys.readouterr().err
    )


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
            "path": item_text(system, "2026-09-01-something"),
        }
    ]


def test_it_needs_an_output():
    assert cli.main(["--notices"]) == cli.EXIT_USAGE


def test_no_registry_is_no_preserved_libraries(system):
    vardb = system.trees[system.eroot]["vartree"].dbapi
    assert notices.preserved_libraries(vardb) == ([], [])


@pytest.mark.skipif(os.geteuid() == 0, reason="root reads anything")
def test_an_unreadable_registry_is_unknown(system, tmp_path):
    from portage.util._dyn_libs.PreservedLibsRegistry import PreservedLibsRegistry

    path = tmp_path / "preserved_libs_registry"
    path.write_text('{"a/b:0": ["a/b-2", "1", ["/usr/lib/liba.so.1"]]}')
    path.chmod(0)
    vardb = system.trees[system.eroot]["vartree"].dbapi
    readable = vardb._plib_registry
    vardb._plib_registry = PreservedLibsRegistry(system.settings["ROOT"], str(path))
    try:
        assert notices.preserved_libraries(vardb) == (None, None)
    finally:
        vardb._plib_registry = readable
    written = json.loads(notices.to_json([], [], None, None))
    assert written["preserved"] is None
    assert written["rebuild"] is None


def rows(lines, kind):
    return [line.split("\t") for line in lines if line.split("\t")[1] == kind]


@needs_egraph
def test_egraph_lists_the_installed_packages_depgraph_finds_masked(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    write_index(scenario, path)
    found = {
        row[0]: tuple(row[2].split(", "))
        for row in rows(notice_lines(scenario, path, tmp_path), "masked")
    }
    masked = update.masked(scenario.trees, scenario.eroot)
    reasons = update.mask_reasons(scenario.trees, scenario.eroot)
    assert found == {
        cpv: reasons[cpv][0] for cpv, is_masked in masked.items() if is_masked
    }


@needs_egraph
def test_egraph_lists_the_sonames_nothing_installed_provides(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    write_index(scenario, path)
    found = {
        (row[0], row[2], row[3])
        for row in rows(notice_lines(scenario, path, tmp_path), "missing")
    }
    vardb = scenario.vardb
    provided = set()
    required = set()
    for cpv in vardb.cpv_all():
        provides, requires = vardb.aux_get(cpv, ["PROVIDES", "REQUIRES"])
        try:
            provided.update(
                (a.multilib_category, a.soname) for a in parse_soname_deps(provides)
            )
            required.update(
                (str(cpv), a.multilib_category, a.soname)
                for a in parse_soname_deps(requires)
            )
        except Exception:
            # A string portage cannot parse, which the store records as an error instead.
            continue
    assert found == {r for r in required if r[1:] not in provided}
