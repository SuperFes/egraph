"""egraph's installed layer against portage, on every scenario and every query."""

import pytest

from compare import QUERIES, assert_agrees, mismatches, subjects
from egraph_build import installed, oracle
from scenarios import SCENARIOS


class _OracleLayer:
    """A layer that answers through the oracle, optionally lying about one query."""

    def __init__(self, vardb, wrong=None):
        self._vardb = vardb
        self._wrong = wrong

    def __getattr__(self, query):
        def answer(*args):
            if query == self._wrong:
                return "wrong"
            return getattr(oracle, query)(self._vardb, *args)

        return answer


@pytest.mark.parametrize("query", QUERIES)
def test_every_query_is_exercised(playgrounds, query):
    assert any(subjects(playgrounds(name).vardb, query) for name in SCENARIOS)


@pytest.mark.parametrize("query", QUERIES)
def test_harness_accepts_agreement(scenario, query):
    assert_agrees(scenario.vardb, _OracleLayer(scenario.vardb), query)


def test_harness_reports_disagreement(playgrounds):
    vardb = playgrounds("reference").vardb
    lines = mismatches(vardb, _OracleLayer(vardb, wrong="rdeps"), "rdeps")
    assert len(lines) == len(oracle.installed(vardb))
    assert lines[0].startswith("rdeps('app-misc/bad-1')\n  portage: ")
    assert lines[0].endswith("\n  egraph:  'wrong'")
    with pytest.raises(AssertionError, match="egraph disagrees with portage"):
        assert_agrees(vardb, _OracleLayer(vardb, wrong="rdeps"), "rdeps")


@pytest.mark.xfail(raises=NotImplementedError, strict=True, reason="roadmap step 2")
@pytest.mark.parametrize("query", QUERIES)
def test_installed_layer_agrees_with_portage(scenario, query):
    layer = installed.build(scenario.vardb)
    assert_agrees(scenario.vardb, layer, query)
