"""The repository index: every version as portage reads it, and its visibility configuration."""

import os
import shutil

import portage
import pytest
from conftest import portdb
from test_build import fresh_databases

from egraph_build import __version__, build, repository, store


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


def test_the_configuration_as_portage_parsed_it(playgrounds):
    index = repository.read(portdb(playgrounds("visibility")))
    vis = index.visibility
    assert vis.accept_keywords == ("x86",)
    assert vis.eapis == (
        repository.Eapi("8", True, False),
        repository.Eapi("99", False, False),
    )
    assert vis.profile_keywords == (
        (repository.Entry("app-misc/keyworded", ("x86",)),),
    )
    assert vis.profile_accept_keywords == (
        (repository.Entry("app-misc/profiled", ("~x86",)),),
    )
    # An empty token list is portage's ~arch default already; wildcards come last.
    entries = vis.accept_keywords_entries
    assert entries[0] == repository.Entry("app-misc/accepted", ("~x86",))
    assert entries[-2:] == (
        repository.Entry("app-misc/wild*", ("~x86",)),
        repository.Entry("*/*::overlay", ("~x86",)),
    )
    assert "app-misc/masked" in vis.masks
    assert "app-misc/profile-masked" in vis.masks
    assert "app-misc/repo-masked::test_repo" in vis.masks
    assert vis.unmasks == ("app-misc/unmasked",)
    # Groups expanded: @EULA holds TEST.
    assert vis.accept_license == ("*", "-EULA", "-TEST")

    assert vis.licenses == (repository.Entry("app-misc/eula-ok", ("EULA",)),)
    assert vis.accept_properties == ("*", "-interactive")
    assert vis.accept_restrict == ("*", "-fetch")


def test_use_only_where_license_or_properties_tests_it(playgrounds):
    index = repository.read(portdb(playgrounds("visibility")))
    use = {v.cpv: v.use for v in index.versions}
    assert use["app-misc/conditional-1"] == ("bin",)
    assert use["app-misc/conditional-off-1"] == ()
    assert use["app-misc/stable-1"] == ()


def test_licenses_are_kept_as_their_net_effect():
    net = repository._net
    assert net(("A", "-B", "*", "-C", "D", "C")) == ("*", "C", "D")
    assert net(("-*", "B", "-B", "A")) == ("-*", "-B", "A")
    assert net(("B", "A", "-A")) == ("-A", "B")


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
    assert (
        repository.Entry("app-misc/testing", ("~x86",))
        in result.index.visibility.accept_keywords_entries
    )


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
