"""egraph against portage on this machine's own /var/db/pkg, read-only."""

import json
import os

import pytest

from compare import QUERIES, assert_agrees
from egraph_build import cli, installed, oracle

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
