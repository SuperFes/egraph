"""Inputs and incremental builds: every incremental result must equal a full build."""

import os
import shutil
import time

import pytest

from egraph_build import build, cli, installed, store
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
    monkeypatch.setattr(cli, "open_vardb", lambda *args: fresh_vardb(playground))
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
