"""Inputs and incremental builds: every incremental result must equal a full build."""

import os
import shutil
import subprocess
import time

import portage
import pytest
from portage.versions import cpv_getkey

from egraph_build import build, cli, evaluated, installed, store
from egraph_build.installed import ATOM

HOUR_NS = 3600 * 10**9


def age(root):
    """Backdate everything under root, so the tests' own edits are not racy."""
    past = time.time_ns() - HOUR_NS
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            os.utime(
                os.path.join(directory, name), ns=(past, past), follow_symlinks=False
            )
    os.utime(root, ns=(past, past))


_open_vardb = cli.open_vardb


def fresh_vardb(playground):
    # A new vardb each time: portage caches matches by directory mtime in whole seconds.
    return _open_vardb(playground.eroot, "/", playground.eprefix)


def write_atomically(path, text):
    temp = path + ".new"
    with open(temp, "w") as f:
        f.write(text)
    os.replace(temp, path)


def add_to_world(playground, atom):
    with open(os.path.join(playground.eroot, "var/lib/portage/world"), "a") as f:
        f.write(atom + "\n")


def vdb(playground, *parts):
    return os.path.join(playground.eroot, "var/db/pkg", *parts)


def add_package(playground, cpv, **metadata):
    directory = vdb(playground, cpv)
    os.makedirs(directory)
    fields = {"EAPI": "8", "SLOT": "0", "repository": "test_repo", **metadata}
    for key, value in fields.items():
        with open(os.path.join(directory, key), "w") as f:
            f.write(value + "\n")


@pytest.fixture
def system(mutable_playground):
    playground = mutable_playground("reference")
    age(playground.eroot)
    first = build.full(fresh_vardb(playground))
    meta = store.Meta("0", "0", playground.eroot, first.started_ns)
    return playground, meta, first


def rebuild(playground, meta, first):
    result = build.incremental(fresh_vardb(playground), meta, first.inputs, first.layer)
    expected = build.full(fresh_vardb(playground))
    assert installed.to_json(result.layer) == installed.to_json(expected.layer)
    assert result.inputs == expected.inputs
    return result


def _nodes(layer, cpv, kind):
    return layer.package(cpv).deps[installed.DEP_KINDS.index(kind)]


def test_inputs_cover_the_vdb_and_the_profile(system):
    playground, _, first = system
    kinds = {item.path: item.kind for item in first.inputs}
    assert kinds[vdb(playground)] == store.INPUT_DIRECTORY
    assert kinds[vdb(playground, "dev-libs")] == store.INPUT_DIRECTORY
    assert kinds[vdb(playground, "dev-libs/lib-1")] == store.INPUT_DIRECTORY
    user = os.path.join(playground.eroot, "etc/portage")
    assert kinds[os.path.join(user, "make.profile")] == store.INPUT_SYMLINK
    assert kinds[os.path.join(user, "make.conf")] == store.INPUT_FILE
    # Absent files are recorded, so creating one later is a change.
    assert kinds[os.path.join(user, "profile")] == store.INPUT_MISSING
    for profile in fresh_vardb(playground).settings.profiles:
        assert kinds[profile] == store.INPUT_DIRECTORY
    world = os.path.join(playground.eroot, "var/lib/portage")
    assert kinds[os.path.join(world, "world")] == store.INPUT_FILE
    assert kinds[os.path.join(world, "world_sets")] == store.INPUT_FILE
    assert kinds[os.path.join(user, "sets")] == store.INPUT_DIRECTORY


def test_nothing_changed(system):
    result = rebuild(*system)
    assert not result.full
    assert result.evaluated == frozenset()


def test_package_changed_in_place(system):
    playground, meta, first = system
    write_atomically(vdb(playground, "app-misc/user-1", "RDEPEND"), "app-misc/old\n")
    result = rebuild(playground, meta, first)
    assert not result.full
    assert result.evaluated == {"app-misc/user-1"}
    (node,) = _nodes(result.layer, "app-misc/user-1", "RDEPEND")
    assert node.matches == ("app-misc/old-1",)


def test_package_added_rematches_its_dependents(system):
    playground, meta, first = system
    add_package(playground, "dev-libs/alt-b-1")
    result = rebuild(playground, meta, first)
    assert not result.full
    assert result.evaluated == {"dev-libs/alt-b-1"}
    alt_b = [
        node
        for node in _nodes(result.layer, "app-misc/root-1", "RDEPEND")
        if node.atom == "dev-libs/alt-b"
    ]
    assert alt_b == [installed.Node(ATOM, 1, "dev-libs/alt-b", ("dev-libs/alt-b-1",))]


def test_package_removed_rematches_its_dependents(system):
    playground, meta, first = system
    shutil.rmtree(vdb(playground, "dev-libs/cond-1"))
    result = rebuild(playground, meta, first)
    assert not result.full
    assert result.evaluated == frozenset()
    assert "dev-libs/cond-1" not in result.layer.installed()
    (node,) = _nodes(result.layer, "app-misc/root-1", "DEPEND")
    assert node.matches == ()


def test_new_category(system):
    playground, meta, first = system
    add_package(playground, "sys-apps/new-1", RDEPEND="dev-libs/lib:2")
    result = rebuild(playground, meta, first)
    assert not result.full
    assert result.evaluated == {"sys-apps/new-1"}


def test_world_edit_only_reads_roots_again(system):
    playground, meta, first = system
    add_to_world(playground, "app-misc/old")
    result = rebuild(playground, meta, first)
    assert not result.full
    assert result.evaluated == frozenset()
    assert ("selected", "app-misc/old", ("app-misc/old-1",)) in result.layer.roots()


def test_package_added_rematches_roots(system):
    playground, meta, first = system
    add_package(playground, "app-misc/user-2")
    result = rebuild(playground, meta, first)
    assert not result.full
    assert (
        "selected",
        "app-misc/user",
        ("app-misc/user-1", "app-misc/user-2"),
    ) in result.layer.roots()


def test_new_user_set_only_reads_roots_again(system):
    playground, meta, first = system
    with open(os.path.join(playground.eroot, "etc/portage/sets/extra"), "w") as f:
        f.write("dev-libs/cond\n")
    with open(os.path.join(playground.eroot, "var/lib/portage/world_sets"), "a") as f:
        f.write("@extra\n")
    result = rebuild(playground, meta, first)
    assert not result.full
    assert ("selected", "dev-libs/cond", ("dev-libs/cond-1",)) in result.layer.roots()


def test_profile_edit_forces_a_full_build(system):
    playground, meta, first = system
    profile = fresh_vardb(playground).settings.profiles[-1]
    with open(os.path.join(profile, "make.defaults"), "a") as f:
        f.write('IUSE_IMPLICIT="${IUSE_IMPLICIT} extra"\n')
    assert rebuild(playground, meta, first).full


def test_created_config_file_forces_a_full_build(system):
    playground, meta, first = system
    os.makedirs(os.path.join(playground.eroot, "etc/portage/profile"))
    assert rebuild(playground, meta, first).full


def test_racy_inputs_are_not_trusted(mutable_playground):
    playground = mutable_playground("reference")
    # Not aged: every input was written within the racy window of the build.
    first = build.full(fresh_vardb(playground))
    meta = store.Meta("0", "0", playground.eroot, first.started_ns)
    assert rebuild(playground, meta, first).full


def test_another_eroot_forces_a_full_build(system):
    playground, meta, first = system
    assert rebuild(playground, meta._replace(eroot="/elsewhere/"), first).full


@pytest.fixture
def cli_system(system, monkeypatch, tmp_path):
    playground, _, _ = system
    monkeypatch.setattr(
        cli,
        "open_databases",
        lambda *args: (
            fresh_vardb(playground),
            playground.trees[playground.eroot]["porttree"].dbapi,
        ),
    )
    path = tmp_path / "installed.egraph"
    assert cli.main(["--full", "--store", str(path)]) == cli.EXIT_OK
    return playground, path


def test_incremental_updates_the_store(cli_system):
    playground, path = cli_system
    add_package(playground, "dev-libs/alt-b-1")
    assert cli.main(["--incremental", "--store", str(path)]) == cli.EXIT_OK
    meta, inputs, layer = store.decode(path.read_bytes())
    assert "dev-libs/alt-b-1" in layer.installed()
    assert vdb(playground, "dev-libs/alt-b-1") in {item.path for item in inputs}


def test_incremental_without_a_store_builds_one(cli_system, tmp_path):
    other = tmp_path / "other.egraph"
    assert cli.main(["--incremental", "--store", str(other)]) == cli.EXIT_OK
    assert store.decode(other.read_bytes())[2].installed()


def test_strict_mode_passes_a_correct_incremental(cli_system, monkeypatch):
    playground, path = cli_system
    monkeypatch.setenv("EGRAPH_STRICT", "1")
    shutil.rmtree(vdb(playground, "dev-libs/cond-1"))
    assert cli.main(["--incremental", "--store", str(path)]) == cli.EXIT_OK


def test_strict_mode_catches_a_wrong_incremental(cli_system, monkeypatch, capsys):
    playground, path = cli_system
    monkeypatch.setenv("EGRAPH_STRICT", "1")
    real = build.incremental

    def forgetful(vardb, meta, inputs, layer):
        # Drops the rematching of unchanged packages.
        result = real(vardb, meta, inputs, layer)
        return result._replace(layer=layer)

    monkeypatch.setattr(build, "incremental", forgetful)
    before = path.read_bytes()
    shutil.rmtree(vdb(playground, "dev-libs/cond-1"))
    assert cli.main(["--incremental", "--store", str(path)]) == cli.EXIT_FAILURE
    assert "differs from a full build" in capsys.readouterr().err
    assert path.read_bytes() == before


def test_evaluated_inputs(playgrounds):
    system = playgrounds("repository")
    portdb = system.trees[system.eroot]["porttree"].dbapi
    cps = sorted({cpv_getkey(str(cpv)) for cpv in system.vardb.cpv_all()})
    paths = {
        item.path: item.kind
        for item in build.evaluated_inputs(system.vardb.settings, portdb, cps)
    }
    main = portdb.getRepositoryPath("test_repo")
    overlay = portdb.getRepositoryPath("overlay")
    user = os.path.join(system.eroot, "etc", "portage")
    assert paths[os.path.join(user, "package.mask")] == store.INPUT_FILE
    assert paths[os.path.join(user, "package.accept_keywords")] == store.INPUT_MISSING
    assert (
        paths[os.path.join(main, "profiles", "updates", "1Q-2026")] == store.INPUT_FILE
    )
    for repo in (main, overlay):
        assert paths[repo] == store.INPUT_DIRECTORY
        assert os.path.join(repo, "eclass") in paths
    # The main repository through its metadata cache only; others down to their ebuilds.
    assert os.path.join(main, "app-misc", "over") not in paths
    assert os.path.join(main, "app-misc", "dyn") not in paths
    for relative in (
        "app-misc",
        "app-misc/over",
        "app-misc/over/over-1.ebuild",
        "app-misc/over/over-2.ebuild",
        "dev-libs/new/new-1.ebuild",
    ):
        assert os.path.join(overlay, relative) in paths, relative
    # Only the installed categories.
    assert all("/sys-apps" not in path for path in paths)


def fresh_databases(playground):
    """A new vardb and portdb each time: portdb keeps the metadata it read. The playground's
    repositories come from the environment, as they do for its own trees."""
    config = os.path.join(playground.eprefix, "etc/portage/repos.conf")
    with open(config) as f:
        env = dict(os.environ, PORTAGE_REPOSITORIES=f.read())
    trees = portage.create_trees(
        config_root=playground.eroot,
        target_root="/",
        eprefix=playground.eprefix,
        env=env,
    )
    tree = trees[trees._target_eroot]
    return tree["vartree"].dbapi, tree["porttree"].dbapi


def repository(playground, name):
    return os.path.join(playground.eprefix, "var/repositories", name)


def egencache(playground, name, *update):
    subprocess.run(
        [
            "egencache",
            f"--repo={name}",
            *update,
            "--sign-manifests=n",
            "--strict-manifests=n",
            f"--repositories-configuration={playground.settings['PORTAGE_REPOSITORIES']}",
        ],
        env=playground.settings.environ(),
        check=True,
    )


def sync(playground, name):
    """What a sync brings: the repository's manifests and metadata cache matching its ebuilds."""
    egencache(playground, name, "--update", "--update-manifests")


def edit_ebuild(playground, name, cpv, old, new):
    cp, _, _ = cpv.rpartition("-")
    path = os.path.join(
        repository(playground, name), cp, f"{cpv.partition('/')[2]}.ebuild"
    )
    with open(path) as f:
        text = f.read()
    assert old in text
    write_atomically(path, text.replace(old, new))


@pytest.fixture
def evaluated_system(mutable_playground):
    """The repository scenario with both stores built, as (playground, installed store,
    evaluated store), each store its (meta, inputs, layer)."""
    playground = mutable_playground("repository")
    age(playground.eroot)
    vardb, portdb = fresh_databases(playground)
    first = build.full(vardb)
    ev = build.evaluate(vardb, portdb)
    eroot = vardb.settings["EROOT"]
    meta = store.Meta("0", "0", eroot, first.started_ns)
    ev_meta = store.EvaluatedMeta("0", "0", eroot, ev.started_ns, first.started_ns)
    return (
        playground,
        (meta, first.inputs, first.layer),
        (ev_meta, ev.inputs, ev.layer),
    )


def reevaluate(playground, previous, previous_evaluated, installed_build_ns=None):
    vardb, portdb = fresh_databases(playground)
    result = build.incremental(vardb, *previous)
    if installed_build_ns is None:
        installed_build_ns = previous[0].build_time_ns
    ev = build.evaluate_incremental(
        vardb, portdb, previous_evaluated, result, installed_build_ns
    )
    expected = build.evaluate(*fresh_databases(playground))
    assert evaluated.to_json(ev.layer) == evaluated.to_json(expected.layer)
    assert ev.inputs == expected.inputs
    return ev


def test_evaluated_nothing_changed(evaluated_system):
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == frozenset()


def test_evaluated_package_added(evaluated_system):
    playground = evaluated_system[0]
    add_package(playground, "app-misc/masked-1")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == {"app-misc/masked"}


def test_evaluated_package_removed_rematches_its_dependents(evaluated_system):
    playground = evaluated_system[0]
    shutil.rmtree(vdb(playground, "dev-libs/new-1"))
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    # Its dependent still needs it, so it is read again as a cp to pull in.
    assert ev.evaluated == {"dev-libs/new"}
    assert "dev-libs/new" in {c.cp for c in ev.layer.candidates()}
    (node,) = ev.layer.package("app-misc/dyn-1").deps[
        installed.DEP_KINDS.index("RDEPEND")
    ]
    assert (node.atom, node.matches) == ("dev-libs/new", ())


def test_evaluated_package_rebuilt_in_place(evaluated_system):
    playground = evaluated_system[0]
    write_atomically(vdb(playground, "app-misc/flags-1", "USE"), "new old\n")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == {"app-misc/flags"}
    assert ev.layer.package("app-misc/flags-1").rebuild == ("-old*",)


def test_evaluated_new_category(evaluated_system):
    playground = evaluated_system[0]
    add_package(playground, "sys-apps/fresh-1", RDEPEND="dev-libs/new")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == {"sys-apps/fresh"}


def test_evaluated_synced_ebuild_reevaluates_its_category(evaluated_system):
    playground = evaluated_system[0]
    edit_ebuild(
        playground, "test_repo", "dev-libs/lib-2.1", 'KEYWORDS="x86"', 'KEYWORDS="~x86"'
    )
    sync(playground, "test_repo")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == {"dev-libs/lib", "dev-libs/new", "dev-libs/old"}
    assert ev.layer.package("dev-libs/lib-2").target is None


def test_evaluated_synced_dependency_reads_the_cp_it_pulls_in(evaluated_system):
    playground = evaluated_system[0]
    main = repository(playground, "test_repo")
    os.makedirs(os.path.join(main, "app-misc/pulled"))
    write_atomically(
        os.path.join(main, "app-misc/pulled/pulled-1.ebuild"),
        'EAPI="8"\nKEYWORDS="x86"\nSLOT="0"\n',
    )
    edit_ebuild(
        playground,
        "test_repo",
        "dev-libs/lib-2.1",
        'KEYWORDS="x86"',
        'KEYWORDS="x86"\nRDEPEND="app-misc/pulled"',
    )
    sync(playground, "test_repo")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert "app-misc/pulled" in ev.evaluated
    assert [c.cpv for c in ev.layer.candidates("app-misc/pulled")] == [
        "app-misc/pulled-1"
    ]


def test_evaluated_overlay_ebuild_reevaluates_its_cp(evaluated_system):
    playground = evaluated_system[0]
    ebuild = os.path.join(
        repository(playground, "overlay"), "app-misc/over/over-3.ebuild"
    )
    write_atomically(ebuild, 'EAPI="8"\nKEYWORDS="x86"\nSLOT="0"\n')
    # Edited in place: the digest, but no metadata cache entry.
    egencache(playground, "overlay", "--update-manifests")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == {"app-misc/over"}
    assert ev.layer.package("app-misc/over-1").target == ("app-misc/over-3", "overlay")


def test_evaluated_eclass_added_to_the_main_repository(evaluated_system):
    playground = evaluated_system[0]
    eclass = os.path.join(repository(playground, "test_repo"), "eclass", "new.eclass")
    write_atomically(eclass, "# nothing inherits it\n")
    ev = reevaluate(*evaluated_system)
    assert not ev.full
    assert ev.evaluated == frozenset()


def test_evaluated_eclass_added_to_an_overlay_is_a_full_build(evaluated_system):
    playground = evaluated_system[0]
    eclass = os.path.join(repository(playground, "overlay"), "eclass", "new.eclass")
    write_atomically(eclass, "# nothing inherits it\n")
    assert reevaluate(*evaluated_system).full


@pytest.mark.parametrize(
    "path, text",
    [
        ("etc/portage/package.mask", "=app-misc/testing-1\n"),
        ("var/repositories/test_repo/profiles/updates/2Q-2026", "move a-b/c a-b/d\n"),
        ("var/repositories/test_repo/profiles/package.mask", "app-misc/over\n"),
    ],
    ids=["user-mask", "move", "repository-mask"],
)
def test_evaluated_global_inputs_force_a_full_build(evaluated_system, path, text):
    playground = evaluated_system[0]
    path = os.path.join(playground.eprefix, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a") as f:
        f.write(text)
    assert reevaluate(*evaluated_system).full


def test_evaluated_full_installed_build_is_a_full_build(evaluated_system):
    playground = evaluated_system[0]
    os.makedirs(os.path.join(playground.eroot, "etc/portage/profile"))
    assert reevaluate(*evaluated_system).full


def test_evaluated_against_another_installed_store_is_a_full_build(evaluated_system):
    playground, previous, previous_evaluated = evaluated_system
    assert reevaluate(playground, previous, previous_evaluated, 1).full


def test_evaluated_racy_inputs_are_not_trusted(evaluated_system):
    playground, previous, (meta, inputs, layer) = evaluated_system
    # Built right after its inputs were last modified.
    newest = max(item.mtime_ns for item in inputs)
    meta = meta._replace(build_time_ns=newest + 1)
    assert reevaluate(playground, previous, (meta, inputs, layer)).full


def test_incremental_updates_the_evaluated_store(cli_system):
    playground, path = cli_system
    add_package(playground, "dev-libs/alt-b-1")
    assert cli.main(["--incremental", "--store", str(path)]) == cli.EXIT_OK
    meta = store.decode(path.read_bytes())[0]
    ev_meta, _, layer = store.decode_evaluated(
        (path.parent / "installed.evaluated.egraph").read_bytes()
    )
    assert "dev-libs/alt-b-1" in layer.installed()
    assert ev_meta.installed_build_time_ns == meta.build_time_ns


def test_strict_mode_catches_a_wrong_evaluated_incremental(
    cli_system, monkeypatch, capsys
):
    playground, path = cli_system
    monkeypatch.setenv("EGRAPH_STRICT", "1")
    real = build.evaluate_incremental

    def forgetful(vardb, portdb, previous, installed_build, installed_build_ns):
        # Keeps the previous dependencies of unchanged packages.
        result = real(vardb, portdb, previous, installed_build, installed_build_ns)
        kept = [previous[2].package(cpv) for cpv in result.layer.installed()]
        return result._replace(
            layer=evaluated.EvaluatedLayer(kept, result.layer.candidates()),
            full=False,
        )

    monkeypatch.setattr(build, "evaluate_incremental", forgetful)
    evaluated_path = path.parent / "installed.evaluated.egraph"
    before = path.read_bytes(), evaluated_path.read_bytes()
    shutil.rmtree(vdb(playground, "dev-libs/cond-1"))
    assert cli.main(["--incremental", "--store", str(path)]) == cli.EXIT_FAILURE
    assert "evaluated store differs from a full build" in capsys.readouterr().err
    assert (path.read_bytes(), evaluated_path.read_bytes()) == before
