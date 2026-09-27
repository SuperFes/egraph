"""egraph against portage on this machine's own /var/db/pkg, read-only."""

import json
import os
import subprocess

import pytest

from compare import QUERIES, assert_agrees
from egraph_build import cli, installed, oracle, store

pytestmark = [
    pytest.mark.system,
    pytest.mark.skipif(
        os.environ.get("EGRAPH_SYSTEM_TESTS") != "1",
        reason="set EGRAPH_SYSTEM_TESTS=1 to read the live /var/db/pkg",
    ),
]

# The oracle evaluates every installed package per rdeps call, so the live run samples.
SAMPLE = int(os.environ.get("EGRAPH_SYSTEM_SAMPLE", "20"))


@pytest.fixture(scope="module")
def live_vardb():
    return cli.open_vardb("/", "/")


@pytest.fixture(scope="module")
def live_layer(live_vardb):
    return installed.build(live_vardb)


def test_oracle_reads_the_live_vdb(live_vardb):
    cpvs = oracle.installed(live_vardb)
    assert cpvs
    for cpv in cpvs:
        assert oracle.matches(live_vardb, f"={cpv}") == (cpv,)


@pytest.mark.parametrize("query", QUERIES)
def test_installed_layer_agrees_with_portage(live_vardb, live_layer, query):
    assert_agrees(live_vardb, live_layer, query, sample=SAMPLE)


def test_json_covers_every_package(live_vardb, live_layer):
    document = json.loads(installed.to_json(live_layer))
    assert [pkg["cpv"] for pkg in document["packages"]] == list(
        oracle.installed(live_vardb)
    )


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_reads_the_live_store(live_layer, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(live_layer, store.Meta("0", "0", "/", 0)))
    exported = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(path), "export", "--format", "json"],
        capture_output=True,
        check=True,
    ).stdout
    assert exported == installed.to_json(live_layer).encode()


def test_incremental_agrees_with_full(live_vardb, tmp_path, monkeypatch):
    monkeypatch.setenv("EGRAPH_STRICT", "1")
    path = tmp_path / "installed.egraph"
    assert cli.main(["--full", "--store", str(path)]) == cli.EXIT_OK
    assert cli.main(["--incremental", "--store", str(path)]) == cli.EXIT_OK
