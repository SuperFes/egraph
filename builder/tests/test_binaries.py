"""The binary packages in the repository index: those portage's indexes list, with what emerge
would refuse them for apart from their USE."""

import os
import shutil
import subprocess

import portage
import pytest
from conftest import portdb
from test_build import age, fresh_databases
from test_golden import EGRAPH, META

from egraph_build import __version__, binaries, build, installed, repository, store

BINARY_EBUILDS = {
    "app-misc/dep-1": {"EAPI": "8", "SLOT": "0/1"},
    "app-misc/a-1": {
        "EAPI": "8",
        "IUSE": "foo +bar",
        "RDEPEND": "foo? ( app-misc/dep ) bar? ( app-misc/dep:= )",
    },
    "app-misc/b-1": {"EAPI": "8", "RDEPEND": "app-misc/dep"},
    "app-misc/p-1": {"EAPI": "8"},
    "sys-libs/glibc-2": {"EAPI": "8"},
    "virtual/libc-1": {"EAPI": "8", "RDEPEND": "sys-libs/glibc"},
}

BINARY_PACKAGES = {
    # Its deps as the ebuild's under its USE, a slot operator's slot and the libc left out.
    "app-misc/a-1": {
        "EAPI": "8",
        "IUSE": "foo +bar",
        "USE": "bar foo",
        "RDEPEND": "app-misc/dep app-misc/dep:0/1= sys-libs/glibc",
        "BUILD_ID": "3",
    },
    # Built before its ebuild gained a dep.
    "app-misc/b-1": {"EAPI": "8", "RDEPEND": ""},
    # No ebuild has its cpv.
    "app-misc/c-2": {"EAPI": "8", "RDEPEND": "app-misc/gone"},
    "app-misc/p-1": {"EAPI": "8"},
}

REMOTE_PACKAGES = {
    "app-misc/a-1": {"EAPI": "8", "IUSE": "foo +bar", "USE": "bar"},
}


def binary_playground():
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    return ResolverPlayground(
        ebuilds=BINARY_EBUILDS,
        installed={
            "sys-libs/glibc-2": {"EAPI": "8"},
            "virtual/libc-1": {"EAPI": "8", "RDEPEND": "sys-libs/glibc"},
        },
        binpkgs=BINARY_PACKAGES,
        binrepos={"remote": REMOTE_PACKAGES},
    )


def cache_remote(playground):
    """The copy of the binhost's index portage keeps once it fetched it."""
    _, db = fresh_databases(playground)
    (repo,) = binaries.binhosts(db.settings)
    cached = binaries.cache_path(db.settings, repo.sync_uri)
    os.makedirs(os.path.dirname(cached), exist_ok=True)
    shutil.copy(os.path.join(repo.sync_uri, "Packages"), cached)
    return cached


@pytest.fixture(scope="module")
def binary_system(gnupg_home):
    playground = binary_playground()
    cache_remote(playground)
    yield playground
    playground.cleanup()


def by_cpv(found):
    return {(b.cpv, b.source): b for b in found}


def test_binaries_are_listed_as_portage_indexes_them(binary_system):
    _, db = fresh_databases(binary_system)
    sources, found = binaries.read(db)
    remote = binaries.binhosts(db.settings)[0].sync_uri
    assert sources == (
        binaries.Source(binaries.LOCAL, "", db.settings["PKGDIR"]),
        binaries.Source(binaries.BINHOST, "remote", remote),
    )
    assert [(b.cpv, b.source) for b in found] == [
        ("app-misc/a-1", 0),
        ("app-misc/a-1", 1),
        ("app-misc/b-1", 0),
        ("app-misc/c-2", 0),
        ("app-misc/p-1", 0),
    ]
    a = by_cpv(found)["app-misc/a-1", 0]
    assert (a.slot, a.build_id, a.use, a.iuse) == (
        "0",
        3,
        ("bar", "foo"),
        ("bar", "foo"),
    )
    assert a.size > 0
    assert by_cpv(found)["app-misc/a-1", 1].use == ("bar",)


def test_binaries_whose_deps_changed_are_marked(binary_system):
    _, db = fresh_databases(binary_system)
    found = by_cpv(binaries.read(db)[1])
    assert found["app-misc/a-1", 0].ignored == 0
    assert found["app-misc/b-1", 0].ignored == binaries.CHANGED_DEPS
    assert found["app-misc/c-2", 0].ignored == 0


def test_a_binary_without_the_user_patches_is_marked(gnupg_home):
    playground = binary_playground()
    try:
        patches = os.path.join(playground.eroot, "etc", "portage", "patches")
        os.makedirs(os.path.join(patches, "app-misc", "p"))
        with open(os.path.join(patches, "app-misc", "p", "fix.patch"), "w") as f:
            f.write("--- a\n+++ b\n")
        _, db = fresh_databases(playground)
        found = by_cpv(binaries.read(db)[1])
        assert found["app-misc/p-1", 0].ignored == binaries.USER_PATCHES
        assert found["app-misc/a-1", 0].ignored == 0
    finally:
        playground.cleanup()


def test_a_binhost_never_fetched_from_lists_nothing(gnupg_home):
    playground = binary_playground()
    try:
        _, db = fresh_databases(playground)
        sources, found = binaries.read(db)
        assert len(sources) == 2
        assert {b.source for b in found} == {0}
    finally:
        playground.cleanup()


class _Repo:
    def __init__(self, exclude=(), include=(), verify=False):
        from portage._sets.base import WildcardPackageSet

        self.getbinpkg_exclude = WildcardPackageSet(exclude)
        self.getbinpkg_include = WildcardPackageSet(include)
        self.verify_signature = verify


def test_a_binhosts_lists_and_signatures_leave_binaries_out(binary_system):
    from portage.versions import _pkg_str

    _, db = fresh_databases(binary_system)
    settings = db.settings
    cpv = _pkg_str("app-misc/a-1", metadata={"SLOT": "0"}, settings=settings)
    entry = {"PATH": "app-misc/a/a-1-1.xpak"}
    admitted = binaries._admitted
    assert admitted(settings, _Repo())(entry, cpv)
    assert not admitted(settings, _Repo(exclude=["app-misc/a"]))(entry, cpv)
    assert not admitted(settings, _Repo(include=["app-misc/b"]))(entry, cpv)
    assert admitted(settings, _Repo(include=["app-misc/a"]))(entry, cpv)
    assert not admitted(settings, _Repo(verify=True))(entry, cpv)
    assert admitted(settings, _Repo(verify=True))(
        {"PATH": "app-misc/a/a-1-1.gpkg.tar"}, cpv
    )


def test_the_index_round_trips_with_binaries(binary_system, tmp_path):
    index = repository.read(portdb(binary_system))
    assert index.binaries
    meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 7)
    assert store.decode_repository(store.encode_repository(index, meta)) == (
        meta,
        (),
        index,
    )
    path = tmp_path / "installed.egraph"
    vardb = binary_system.trees[binary_system.eroot]["vartree"].dbapi
    store.write(path, store.encode(installed.build(vardb), META))
    store.write(store.repository_path(path), store.encode_repository(index, meta))
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "export", "--format", "json"]
        + ["--repository"],
        capture_output=True,
    )
    assert result.stderr == b""
    assert result.stdout == repository.to_json(index).encode()


@pytest.fixture
def aged_binary_system(gnupg_home):
    playground = binary_playground()
    cache_remote(playground)
    age(playground.eroot)
    yield playground
    playground.cleanup()


def first_index(playground):
    _, db = fresh_databases(playground)
    first = build.index(db)
    meta = store.RepositoryMeta(
        __version__, portage.VERSION, db.settings["EROOT"], first.started_ns
    )
    return meta, first.inputs, first.index


def rebuilt(playground, previous):
    _, db = fresh_databases(playground)
    result = build.index_incremental(db, previous)
    assert repository.to_json(result.index) == repository.to_json(repository.read(db))
    return result


def test_a_changed_binary_index_reads_only_the_binaries_again(aged_binary_system):
    previous = first_index(aged_binary_system)
    cached = cache_remote(aged_binary_system)
    with open(cached) as f:
        text = f.read()
    with open(cached, "w") as f:
        f.write(text.replace("USE: bar\n", "USE: bar foo\n"))
    result = rebuilt(aged_binary_system, previous)
    assert not result.full
    assert result.reread == frozenset()
    assert by_cpv(result.index.binaries)["app-misc/a-1", 1].use == ("bar", "foo")


def test_new_user_patches_read_every_binary_again(aged_binary_system):
    previous = first_index(aged_binary_system)
    patches = os.path.join(aged_binary_system.eroot, "etc", "portage", "patches")
    os.makedirs(os.path.join(patches, "app-misc", "p"))
    with open(os.path.join(patches, "app-misc", "p", "fix.patch"), "w") as f:
        f.write("--- a\n+++ b\n")
    result = rebuilt(aged_binary_system, previous)
    assert not result.full
    found = by_cpv(result.index.binaries)
    assert found["app-misc/p-1", 0].ignored == binaries.USER_PATCHES


def test_a_cp_read_again_reads_its_binaries_again(aged_binary_system):
    # A sync replaces the ebuild and adds a package beside it, which the category shows.
    previous = first_index(aged_binary_system)
    _, db = fresh_databases(aged_binary_system)
    ebuild = os.path.join(db.getRepositoryPath("test_repo"), "app-misc/b/b-1.ebuild")
    with open(ebuild) as f:
        text = f.read()
    with open(ebuild, "w") as f:
        f.write(text.replace('RDEPEND="app-misc/dep"', 'RDEPEND=""'))
    os.makedirs(os.path.join(os.path.dirname(os.path.dirname(ebuild)), "new"))
    result = rebuilt(aged_binary_system, previous)
    assert not result.full
    assert "app-misc/b" in result.reread
    assert by_cpv(result.index.binaries)["app-misc/b-1", 0].ignored == 0
