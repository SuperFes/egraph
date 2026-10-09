"""The repository index: every version as portage reads it, and what visibility is decided from."""

import itertools
import json
import os
import shutil
import subprocess

import portage
import managers
import pytest
from conftest import portdb
from test_build import fresh_databases
from portage.versions import cpv_getkey
from test_ledger import SHADOW, stacked_in_cpp

from egraph_build import __version__, build, installed, repository, store


def test_every_version_in_every_repository_as_portage_reads_it(scenario):
    db = portdb(scenario)
    index = repository.read(db)
    expected = [
        (cpv, repo)
        for cp in sorted(db.cp_all())
        for repo in db.getRepositories()
        for cpv in db.cp_list(cp, mytree=db.getRepositoryPath(repo))
    ]
    assert [(v.cpv, v.repo) for v in index.versions] == expected
    for v in index.versions:
        slot, eapi, keywords, license_, description = db.aux_get(
            v.cpv, ["SLOT", "EAPI", "KEYWORDS", "LICENSE", "DESCRIPTION"], myrepo=v.repo
        )
        assert (v.slot, v.sub_slot) == (
            slot.partition("/")[0],
            slot.partition("/")[2] or slot.partition("/")[0],
        )
        assert (v.eapi, " ".join(v.keywords), " ".join(v.license)) == (
            eapi,
            " ".join(keywords.split()),
            " ".join(license_.split()),
        )
        assert v.description == description
    assert [(r.name, r.location) for r in index.repositories] == [
        (name, db.getRepositoryPath(name)) for name in db.getRepositories()
    ]


def test_the_index_round_trips(scenario):
    index = repository.read(portdb(scenario))
    meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 7)
    inputs = (store.Input("/x", store.INPUT_FILE, 1, 2),)
    assert store.decode_repository(store.encode_repository(index, meta, inputs)) == (
        meta,
        inputs,
        index,
    )


def test_the_configuration_stacks_as_portage_parsed_it(playgrounds, tmp_path):
    db = portdb(playgrounds("visibility"))
    assert repository.read(db).visibility.eapis == (
        repository.Eapi("8", True, False),
        repository.Eapi("99", False, False),
    )
    vis = stacked_in_cpp(db, tmp_path)
    assert vis["accept_keywords"] == ["x86"]
    assert vis["profile_keywords"] == [
        [{"atom": "app-misc/keyworded", "tokens": ["x86"]}]
    ]
    assert vis["profile_accept_keywords"] == [
        [{"atom": "app-misc/profiled", "tokens": ["~x86"]}]
    ]
    # An empty token list is portage's ~arch default; wildcards come last.
    entries = vis["accept_keywords_entries"]
    assert entries[0] == {"atom": "app-misc/accepted", "tokens": ["~x86"]}
    assert entries[-2:] == [
        {"atom": "app-misc/wild*", "tokens": ["~x86"]},
        {"atom": "*/*::overlay", "tokens": ["~x86"]},
    ]
    assert "app-misc/masked" in vis["masks"]
    assert "app-misc/profile-masked" in vis["masks"]
    assert "app-misc/repo-masked::test_repo" in vis["masks"]
    assert vis["unmasks"] == ["app-misc/unmasked"]
    # Groups expanded: @EULA holds TEST.
    assert vis["accept_license"] == ["*", "-EULA", "-TEST"]
    assert vis["licenses"] == [{"atom": "app-misc/eula-ok", "tokens": ["EULA"]}]
    assert vis["accept_properties"] == ["*", "-interactive"]
    assert vis["accept_restrict"] == ["*", "-fetch"]


def test_use_only_where_license_or_properties_tests_it(playgrounds):
    index = repository.read(portdb(playgrounds("visibility")))
    use = {v.cpv: v.use for v in index.versions}
    assert use["app-misc/conditional-1"] == ("bin",)
    assert use["app-misc/conditional-off-1"] == ()
    assert use["app-misc/stable-1"] == ()


def test_licenses_are_compared_as_their_net_effect():
    net = managers.net
    assert net(("A", "-B", "*", "-C", "D", "C")) == ["*", "C", "D"]
    assert net(("-*", "B", "-B", "A")) == ["-*", "-B", "A"]
    assert net(("B", "A", "-A")) == ["-A", "B"]


@pytest.fixture
def repository_playground(mutable_playground):
    from test_build import age

    playground = mutable_playground("repository")
    age(playground.eroot)
    return playground


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


def overlay(playground, *parts):
    _, db = fresh_databases(playground)
    return os.path.join(db.getRepositoryPath("overlay"), *parts)


def test_an_unchanged_index_reads_nothing_again(repository_playground):
    result = rebuilt(repository_playground, first_index(repository_playground))
    assert not result.full
    assert result.reread == frozenset()


def test_a_removed_ebuild_reads_its_cp_again(repository_playground):
    previous = first_index(repository_playground)
    os.unlink(overlay(repository_playground, "app-misc", "over", "over-2.ebuild"))
    result = rebuilt(repository_playground, previous)
    assert not result.full
    assert result.reread == {"app-misc/over"}


def test_a_removed_package_reads_its_category_again(repository_playground):
    previous = first_index(repository_playground)
    shutil.rmtree(overlay(repository_playground, "dev-libs", "new"))
    result = rebuilt(repository_playground, previous)
    assert not result.full
    assert "dev-libs/new" in result.reread
    assert all(cp.startswith("dev-libs/") for cp in result.reread)


def test_a_configuration_change_reads_no_metadata_again(repository_playground):
    previous = first_index(repository_playground)
    path = os.path.join(
        repository_playground.eroot, "etc/portage/package.accept_keywords"
    )
    with open(path, "a") as f:
        f.write("app-misc/testing ~x86\n")
    result = rebuilt(repository_playground, previous)
    assert not result.full
    assert result.reread == frozenset()
    assert ("app-misc/testing", ("~x86",)) in [
        (e.atom, e.tokens) for e in result.index.ledger.package_accept_keywords
    ]


def test_a_user_license_group_change_is_read(repository_playground):
    """A change to an input is what makes egraph rebuild the index."""
    path = os.path.join(repository_playground.eroot, "etc/portage/license_groups")
    with open(path, "w") as f:
        f.write("MINE EULA\n")
    _, inputs, index = first_index(repository_playground)
    assert path in {item.path for item in inputs}
    assert index.ledger.license_groups[-1].var == "MINE"


def test_a_repository_unmask_change_is_read(repository_playground):
    _, db = fresh_databases(repository_playground)
    path = os.path.join(db.getRepositoryPath("test_repo"), "profiles", "package.unmask")
    with open(path, "w") as f:
        f.write("app-misc/testing\n")
    _, inputs, index = first_index(repository_playground)
    assert path in {item.path for item in inputs}
    (repo,) = [r for r in index.ledger.repositories if r.name == "test_repo"]
    assert [e.atom for e in repo.package_unmask] == ["app-misc/testing"]


def test_an_overlay_eclass_change_reads_its_packages_again(repository_playground):
    previous = first_index(repository_playground)
    eclass = overlay(repository_playground, "eclass")
    os.makedirs(eclass, exist_ok=True)
    with open(os.path.join(eclass, "new.eclass"), "w") as f:
        f.write("# new\n")
    result = rebuilt(repository_playground, previous)
    assert not result.full
    assert result.reread == {"app-misc/over", "dev-libs/new"}


def test_a_make_conf_change_reads_everything_again(repository_playground):
    previous = first_index(repository_playground)
    path = os.path.join(repository_playground.eroot, "etc/portage/make.conf")
    with open(path, "a") as f:
        f.write('USE="${USE} new"\n')
    assert rebuilt(repository_playground, previous).full


def test_an_invalid_ebuild_carries_what_depgraph_finds_wrong(playgrounds):
    index = repository.read(portdb(playgrounds("refused")))
    # Conditionals on flags outside IUSE.
    assert {v.cpv for v in index.versions if v.invalid} == {
        "app-misc/inv-1",
        "app-misc/invfb-2",
        "app-misc/invupd-2",
    }


FETCH_EBUILDS = {
    "app-misc/plain-1": {"EAPI": "8", "SRC_URI": "https://example.org/plain-1.tar.gz"},
    "app-misc/cond-1": {
        "EAPI": "8",
        "IUSE": "doc l10n_de",
        "SRC_URI": "https://example.org/cond-1.tar.gz"
        " doc? ( https://example.org/cond-doc-1.tar.gz"
        " l10n_de? ( https://example.org/de.xpi -> cond-de-1.xpi ) )"
        " !doc? ( https://example.org/cond-nodoc-1.tar.gz )"
        " x86? ( https://example.org/cond-x86-1.tar.gz )"
        " l10n_de? ( https://example.org/cond-x86-1.tar.gz )",
        "MISC_CONTENT": "inherit cmake toolchain-funcs\n",
    },
    "app-misc/cond-2": {
        "EAPI": "8",
        "IUSE": "doc",
        "SRC_URI": "https://example.org/cond-2.tar.gz"
        " doc? ( https://example.org/cond-doc-1.tar.gz )",
        "MISC_CONTENT": "inherit cmake\n",
    },
    "app-misc/none-1": {"EAPI": "8"},
}

FETCH_DISTFILES = {
    "plain-1.tar.gz": 100,
    "cond-1.tar.gz": 1000,
    "cond-doc-1.tar.gz": 200,
    "cond-de-1.xpi": 30,
    "cond-nodoc-1.tar.gz": 7,
    "cond-x86-1.tar.gz": 4,
    "cond-2.tar.gz": 2000,
}


@pytest.fixture(scope="module")
def fetch_playground(gnupg_home):
    """A repository whose ebuilds fetch distfiles under conditionals, Manifests and all."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(
        ebuilds=FETCH_EBUILDS,
        distfiles={name: b"x" * size for name, size in FETCH_DISTFILES.items()},
        eclasses={"cmake": ["# cmake"], "toolchain-funcs": ["# toolchain-funcs"]},
    )
    yield playground
    playground.cleanup()


@pytest.fixture(scope="module")
def fetch_db(fetch_playground):
    return portdb(fetch_playground)


def test_fetch_groups_follow_the_conditionals_over_each_file():
    sizes = {"a.tar.gz": 10, "b.tar.gz": 5, "renamed": 3, "c": 1}
    groups = repository.fetch_groups(
        "https://x/a.tar.gz doc? ( mirror://y/b.tar.gz https://z/q -> renamed"
        " !x86? ( c ) ) ( https://x/a.tar.gz ) doc? ( b.tar.gz ) missing.tar.gz"
        " other? ( missing.tar.gz )",
        sizes,
    )
    assert groups == (
        repository.FetchGroup(((),), 10),
        repository.FetchGroup((("doc?",),), 8),
        repository.FetchGroup((("doc?", "!x86?"),), 1),
    )
    # A file in two places, fetched once when either's conditionals are met.
    assert repository.fetch_groups(
        "de? ( a.tar.gz b.tar.gz ) de-AT? ( a.tar.gz ) de-CH? ( b.tar.gz )", sizes
    ) == (
        repository.FetchGroup((("de?",), ("de-AT?",)), 10),
        repository.FetchGroup((("de?",), ("de-CH?",)), 5),
    )
    assert repository.fetch_groups("", sizes) == ()


def test_each_version_carries_its_eclasses_and_distfiles(fetch_db):
    index = repository.read(fetch_db)
    versions = {v.cpv: v for v in index.versions}
    for v in index.versions:
        (inherited,) = fetch_db.aux_get(v.cpv, ["INHERITED"], myrepo=v.repo)
        assert v.inherited == tuple(inherited.split())
    assert set(versions["app-misc/cond-1"].inherited) == {"cmake", "toolchain-funcs"}
    assert versions["app-misc/cond-1"].fetch == (
        repository.FetchGroup(((),), 1000),
        repository.FetchGroup((("doc?",),), 200),
        repository.FetchGroup((("doc?", "l10n_de?"),), 30),
        repository.FetchGroup((("!doc?",),), 7),
        repository.FetchGroup((("x86?",), ("l10n_de?",)), 4),
    )
    assert versions["app-misc/plain-1"].fetch == (repository.FetchGroup(((),), 100),)
    assert versions["app-misc/none-1"].fetch == ()
    meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 7)
    assert store.decode_repository(store.encode_repository(index, meta))[2] == index


def portage_distfiles(db, cpv, repo, use):
    """The bytes of cpv's fetch map under use, from its Manifest, as portage reckons them."""
    location = db.getRepositoryPath(repo)
    files = db.getFetchMap(cpv, useflags=set(use), mytree=location)
    manifest = db.repositories.get_repo_for_location(location).load_manifest(
        os.path.join(location, cpv_getkey(cpv)), db.settings["DISTDIR"]
    )
    return manifest.getDistfilesSize(files)


def distfiles_in_cpp(db, cases, tmp_path):
    """egraph's distfile bytes for each (cpv, repo, use) of cases, from db's index."""
    if not SHADOW:
        pytest.skip("set EGRAPH_SHADOW to the shadow binary (meson test does)")
    path = tmp_path / "index.egraph"
    meta = store.RepositoryMeta("0", "0", "/", 0)
    store.write(path, store.encode_repository(repository.read(db), meta))
    request = {
        "check": "distfiles",
        "index": str(path),
        "cases": [{"cpv": c, "repo": r, "use": list(u)} for c, r, u in cases],
    }
    result = subprocess.run(
        [SHADOW], input=json.dumps(request) + "\n", capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)["bytes"]


def test_a_versions_distfiles_under_a_use_are_its_fetch_maps(fetch_db, tmp_path):
    flags = ("doc", "l10n_de", "x86")
    cases = [
        (cpv, "test_repo", use)
        for cp in fetch_db.cp_all()
        for cpv in fetch_db.cp_list(cp)
        for n in range(len(flags) + 1)
        for use in itertools.combinations(flags, n)
    ]
    assert distfiles_in_cpp(fetch_db, cases, tmp_path) == [
        portage_distfiles(fetch_db, cpv, repo, use) for cpv, repo, use in cases
    ]


def test_cpp_exports_each_versions_eclasses_and_distfiles(fetch_playground, tmp_path):
    from test_golden import EGRAPH, META

    path = tmp_path / "installed.egraph"
    vardb = fetch_playground.trees[fetch_playground.eroot]["vartree"].dbapi
    store.write(path, store.encode(installed.build(vardb), META))
    index = repository.read(portdb(fetch_playground))
    meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 0)
    store.write(store.repository_path(path), store.encode_repository(index, meta))
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "export", "--format", "json"]
        + ["--repository"],
        capture_output=True,
    )
    assert result.stderr == b""
    assert result.stdout == repository.to_json(index).encode()
