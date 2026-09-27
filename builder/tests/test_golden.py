"""The C++ reader against the Python writer: the same store must export the same JSON bytes."""

import os
import subprocess

import pytest

from egraph_build import installed, store

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
        [EGRAPH, "--store", str(path), "export", "--format", "json"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 1
    assert result.stdout == ""
    assert result.stderr.startswith(f"egraph: {path}: ")
