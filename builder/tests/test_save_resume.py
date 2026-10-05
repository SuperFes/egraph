"""egraph-build --save-resume and the worker's drops: mtimedb's resume entry as emerge's scheduler
keeps it."""

import json

import pytest

from egraph_build import cli, resume


def task(cpv, eroot="/"):
    return ["ebuild", eroot, cpv, "merge"]


def entry(*cpvs, eroot="/"):
    return {
        "binpkgs": [],
        "favorites": ["app-misc/a"],
        "mergelist": [task(cpv, eroot) for cpv in cpvs],
        "myopts": {"--regex-search-auto": "y"},
    }


def read(path):
    with open(path) as f:
        return json.load(f)


@pytest.fixture
def mtimedb(tmp_path):
    path = tmp_path / "mtimedb"
    path.write_text(json.dumps({"ldpath": {"/usr/lib64": 1}, "updates": {"x": 2}}))
    return str(path)


@pytest.mark.parametrize(
    "text",
    [
        "[",
        "[]",
        '{"favorites": [], "binpkgs": [], "mergelist": []}',
        '{"myopts": {}, "binpkgs": [], "mergelist": []}',
        '{"myopts": {}, "favorites": [], "mergelist": []}',
        '{"myopts": {}, "favorites": [], "binpkgs": [], "mergelist": {}}',
        '{"myopts": {}, "favorites": [], "binpkgs": [], "mergelist": [["ebuild", "/"]]}',
        '{"myopts": {}, "favorites": [], "binpkgs": [], "mergelist": [[1, 2, 3, 4]]}',
    ],
)
def test_a_malformed_entry_is_refused(text):
    with pytest.raises(ValueError):
        resume.parse_entry(text)


def test_an_entry_is_read_as_written():
    written = entry("app-misc/a-1")
    assert resume.parse_entry(json.dumps(written)) == written


def test_saving_keeps_the_rest_of_the_mtimedb(mtimedb):
    resume.save(mtimedb, entry("app-misc/a-1"), backup=False)
    saved = read(mtimedb)
    assert saved["resume"] == entry("app-misc/a-1")
    assert saved["ldpath"] == {"/usr/lib64": 1}
    assert saved["updates"] == {"x": 2}


def test_a_run_starting_keeps_a_longer_list_from_before_as_backup(mtimedb):
    """As emerge does before it saves its own: a list of more than one merge."""
    before = entry("app-misc/a-1", "app-misc/b-1")
    resume.save(mtimedb, before, backup=False)
    resume.save(mtimedb, entry("app-misc/c-1"), backup=True)
    assert read(mtimedb)["resume_backup"] == before
    assert read(mtimedb)["resume"] == entry("app-misc/c-1")
    resume.save(mtimedb, entry("app-misc/d-1"), backup=True)
    assert read(mtimedb)["resume_backup"] == before
    assert read(mtimedb)["resume"] == entry("app-misc/d-1")


def test_a_run_going_on_replaces_its_list(mtimedb):
    resume.save(mtimedb, entry("app-misc/a-1", "app-misc/b-1"), backup=False)
    resume.save(mtimedb, entry("app-misc/b-1"), backup=False)
    assert "resume_backup" not in read(mtimedb)
    assert read(mtimedb)["resume"] == entry("app-misc/b-1")


def test_a_merge_drops_its_task_and_the_last_the_entry(mtimedb):
    import portage

    resume.save(mtimedb, entry("app-misc/a-1", "app-misc/b-1"), backup=False)
    db = portage.MtimeDB(mtimedb)
    resume.drop(db, "/", "app-misc/a-1")
    assert db["resume"]["mergelist"] == [task("app-misc/b-1")]
    # Another root's, or a package not listed, stays as it is.
    resume.drop(db, "/mnt/", "app-misc/b-1")
    resume.drop(db, "/", "app-misc/c-1")
    assert db["resume"]["mergelist"] == [task("app-misc/b-1")]
    resume.drop(db, "/", "app-misc/b-1")
    assert "resume" not in db
    resume.drop(db, "/", "app-misc/b-1")


@pytest.fixture(scope="module")
def system(gnupg_home):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground()
    yield playground
    playground.cleanup()


def run(system, *argv):
    return cli.main(
        [
            "--save-resume",
            *argv,
            "--config-root",
            system.eroot,
            "--eprefix",
            system.eprefix,
        ]
    )


def test_the_entry_goes_to_the_roots_mtimedb(system, tmp_path):
    path = tmp_path / "entry"
    written = entry("app-misc/a-1", eroot=system.eroot)
    path.write_text(json.dumps(written))
    assert run(system, "--input", str(path)) == cli.EXIT_OK
    assert read(resume.mtimedb_path(system.eroot))["resume"] == written


def test_without_an_entry_to_save_nothing_is_saved(system, tmp_path):
    assert run(system) == cli.EXIT_USAGE
    path = tmp_path / "entry"
    path.write_text("{}")
    assert run(system, "--input", str(path)) == cli.EXIT_FAILURE
    assert run(system, "--input", str(tmp_path / "missing")) == cli.EXIT_FAILURE
