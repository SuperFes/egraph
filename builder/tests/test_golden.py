"""The C++ reader against the Python writer: the same store must export the same JSON bytes."""

import os
import subprocess

import pytest

from egraph_build import evaluated, installed, store

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

META = store.Meta("0.0.0", "3.0.0", "/", 0)


def export(path):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "export", "--format", "json"],
        capture_output=True,
        check=True,
    ).stdout


def test_cpp_export_matches_builder_json(scenario, tmp_path):
    layer = installed.build(scenario.vardb)
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(layer, META))
    assert export(path) == installed.to_json(layer).encode()


def test_a_link_named_for_a_command_runs_it(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(installed.build(scenario.vardb), META))
    link = tmp_path / "egraph-export"
    link.symlink_to(os.path.abspath(EGRAPH))
    linked = subprocess.run(
        [link, "--format", "json", "--store", str(path), "--no-refresh"],
        capture_output=True,
        check=True,
    ).stdout
    assert linked == export(path)


def write_both(system, path, installed_build_time_ns=0):
    store.write(path, store.encode(installed.build(system.vardb), META))
    portdb = system.trees[system.eroot]["porttree"].dbapi
    layer = evaluated.build(system.vardb, portdb)
    meta = store.EvaluatedMeta("0.0.0", "3.0.0", "/", 0, installed_build_time_ns)
    store.write(store.evaluated_path(path), store.encode_evaluated(layer, meta))
    return layer


def export_evaluated(path):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "export", "--format", "json"]
        + ["--evaluated"],
        capture_output=True,
    )


def test_cpp_evaluated_export_matches_builder_json(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    layer = write_both(scenario, path)
    result = export_evaluated(path)
    assert result.stderr == b""
    assert result.stdout == evaluated.to_json(layer).encode()


def test_an_evaluated_store_from_another_build_is_stale(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_both(playgrounds("repository"), path, installed_build_time_ns=1)
    result = export_evaluated(path)
    assert result.returncode == 0
    assert b"stale store (built against another installed store)" in result.stderr


def test_a_missing_evaluated_store_fails_cleanly(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_both(playgrounds("repository"), path)
    os.unlink(store.evaluated_path(path))
    result = export_evaluated(path)
    assert result.returncode == 1
    assert result.stdout == b""
    expected = f"egraph: {store.evaluated_path(path)}: "
    assert result.stderr.decode().startswith(expected)


def test_undecodable_bytes_survive_the_round_trip(tmp_path):
    pkg = installed.Package(
        cpv="app-misc/odd-1",
        cp="app-misc/odd",
        slot="0",
        sub_slot="0",
        repo="r\udcffé",
        eapi="8",
        use=(),
        iuse=(),
        errors=(("RDEPEND", 'tab\there "quoted" \U0001f600'),),
        deps=((),) * len(installed.DEP_KINDS),
        provides=(),
        requires=(),
    )
    layer = installed.InstalledLayer([pkg])
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(layer, META))
    assert export(path) == installed.to_json(layer).encode()


def test_corrupt_store_fails_cleanly(scenario, tmp_path):
    data = store.encode(installed.build(scenario.vardb), META)
    path = tmp_path / "installed.egraph"
    path.write_bytes(data[: len(data) // 2])
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "export", "--format", "json"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 1
    assert result.stdout == ""
    assert result.stderr.startswith(f"egraph: {path}: ")


def test_corrupt_store_is_rebuilt(tmp_path):
    path = tmp_path / "installed.egraph"
    path.write_bytes(b"EGRAPH\0\0garbage")
    log = tmp_path / "args"
    builder = tmp_path / "egraph-build"
    builder.write_text(f'#!/bin/sh\necho "$@" > "{log}"\nexit 3\n')
    builder.chmod(0o755)
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--builder", str(builder)]
        + ["export", "--format", "json"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 1
    assert log.read_text().startswith(f"--incremental --store {path} ")
    assert result.stderr == f"egraph: {builder} exited with status 3\n"
