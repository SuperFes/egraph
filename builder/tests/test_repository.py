"""The repository index: every version as portage reads it, and its visibility configuration."""

from conftest import portdb

from egraph_build import repository, store


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
    assert index.repositories == tuple(
        (name, db.getRepositoryPath(name)) for name in db.getRepositories()
    )


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
