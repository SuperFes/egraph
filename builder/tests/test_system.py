"""egraph against portage on this machine's own /var/db/pkg, read-only."""

import os

import pytest

import portage
from portage.dbapi.vartree import vartree

from compare import QUERIES, assert_agrees
from egraph_build import installed, oracle

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
    settings = portage.config(config_root="/", target_root="/")
    return vartree(settings=settings).dbapi


def test_oracle_reads_the_live_vdb(live_vardb):
    cpvs = oracle.installed(live_vardb)
    assert cpvs
    for cpv in cpvs:
        assert oracle.matches(live_vardb, f"={cpv}") == (cpv,)


@pytest.mark.xfail(raises=NotImplementedError, strict=True, reason="roadmap step 2")
@pytest.mark.parametrize("query", QUERIES)
def test_installed_layer_agrees_with_portage(live_vardb, query):
    layer = installed.build(live_vardb)
    assert_agrees(live_vardb, layer, query, sample=SAMPLE)
