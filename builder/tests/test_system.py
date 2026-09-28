"""egraph against portage on this machine's own /var/db/pkg, read-only."""

import json
import os
import random
import subprocess

import pytest

from compare import QUERIES, assert_agrees
from egraph_build import cli, evaluated, installed, oracle, store

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


@pytest.fixture(scope="module")
def live_store(live_vardb, live_layer, tmp_path_factory):
    from egraph_build.profile import implicit_iuse

    path = tmp_path_factory.mktemp("live") / "installed.egraph"
    meta = store.Meta("0", "0", "/", 0, implicit_iuse(live_vardb.settings))
    store.write(path, store.encode(live_layer, meta))
    return path


def _egraph(path, *args):
    result = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(path), "--no-refresh", *args],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    return result.stdout


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_queries_agree_with_portage(live_vardb, live_store):
    from test_queries import parse_edges

    for (cpv,) in subjects_sample(live_vardb, "deps"):
        assert parse_edges(_egraph(live_store, "deps", cpv)) == oracle.deps(
            live_vardb, cpv
        )
        assert parse_edges(_egraph(live_store, "rdeps", cpv)) == oracle.rdeps(
            live_vardb, cpv
        )


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_broken_agrees_with_portage(live_vardb, live_store):
    expected = sorted("\t".join(item) for item in oracle.broken(live_vardb))
    assert _egraph(live_store, "broken").splitlines() == expected


def subjects_sample(vardb, query):
    import random

    from compare import subjects

    chosen = subjects(vardb, query)
    return sorted(random.Random(query).sample(chosen, min(SAMPLE, len(chosen))))


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_matcher_agrees_on_every_live_atom(live_layer, live_store):
    """Every atom in the live trees, against the matches portage resolved into the store."""
    from test_match import egraph_matches

    expected = {}
    for pkg in live_layer:
        for nodes in pkg.deps:
            for node in nodes:
                if node.atom:
                    expected[node.atom.lstrip("!")] = set(node.matches)
    found = egraph_matches(live_store, sorted(expected))
    wrong = [
        f"{atom}: portage {sorted(expected[atom])}, egraph {sorted(found[atom])}"
        for atom in sorted(expected)
        if found[atom] != expected[atom]
    ]
    assert not wrong, f"{len(wrong)} of {len(expected)} atoms differ:\n" + "\n".join(
        wrong[:20]
    )


@pytest.fixture(scope="module")
def live_emerge_config():
    from _emerge.actions import load_emerge_config

    return load_emerge_config()


@pytest.fixture(scope="module")
def live_depclean(live_emerge_config):
    from depclean import depclean

    config = live_emerge_config
    return {
        with_bdeps: depclean(config.trees, config.target_config.root, with_bdeps)
        for with_bdeps in (True, False)
    }


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_orphans_with_dynamic_deps(live_store, live_emerge_config):
    """emerge's default re-reads dependencies from the repository, which egraph does not; on a
    system whose ebuilds have not dropped a dependency since merge, the answers still agree.
    """
    from depclean import depclean

    config = live_emerge_config
    expected = depclean(config.trees, config.target_config.root, dynamic_deps=True)
    result = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(live_store), "--no-refresh", "orphans"],
        capture_output=True,
        text=True,
    )
    assert tuple(result.stdout.splitlines()) == expected.orphans


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_orphans_are_what_depclean_removes(live_store, live_depclean, with_bdeps):
    expected = live_depclean[with_bdeps]
    result = subprocess.run(
        [
            os.environ["EGRAPH"],
            "--store",
            str(live_store),
            "--no-refresh",
            "orphans",
            "--with-bdeps",
            "y" if with_bdeps else "n",
        ],
        capture_output=True,
        text=True,
    )
    assert (result.returncode != 0) == (expected.returncode != 0), result.stderr
    assert tuple(result.stdout.splitlines()) == expected.orphans


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_why_explains_every_kept_package(live_vardb, live_store, live_depclean):
    from test_why import assert_explains, why

    expected = live_depclean[True]
    for cpv in sorted(expected.kept):
        result = why(live_store, cpv)
        assert result.returncode == 0, result.stderr
        assert_explains(result.stdout, cpv, expected, live_vardb)


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_affected_agrees_with_the_fork_on_the_live_vdb(
    live_vardb, live_layer, tmp_path
):
    graph = pytest.importorskip("portage.dbapi._InstalledGraph")
    neighborhood = pytest.importorskip("_emerge.resolver.neighborhood")

    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(live_layer, store.Meta("0", "0", "/", 0)))
    index = graph.InstalledGraph.from_vardb(live_vardb)

    def ask(**request):
        result = subprocess.run(
            [os.environ["EGRAPH"], "--store", str(path), "affected"],
            input=json.dumps(request),
            capture_output=True,
            text=True,
            check=True,
        )
        return json.loads(result.stdout)

    kinds = ["RDEPEND", "IDEPEND", "PDEPEND", "DEPEND", "BDEPEND", "SONAME"]
    cpvs = sorted(pkg.cpv for pkg in index)
    cps = sorted(
        {pkg.cp for pkg in index} | {edge.atom.cp for pkg in index for edge in pkg.deps}
    )
    blockers = sorted(
        {str(edge.atom) for pkg in index for edge in pkg.deps if edge.atom.blocker}
    )
    blocked = sorted({c for a in blockers for c in index.matches(graph.Atom(a))})
    everything = ask(
        kinds=kinds, seeds=cpvs, changed=cps, replaced=cpvs, blockers=blockers
    )
    assert everything["reachable"] == sorted(index.reachable(cpvs, set(kinds)))
    assert everything["blocked"] == blocked
    assert everything["affected"] == sorted(
        neighborhood.affected_cpvs(index, cps, cpvs, blocked)
    )
    rng = random.Random(0)
    for cpv in rng.sample(cpvs, min(SAMPLE, len(cpvs))):
        answer = ask(
            kinds=kinds, seeds=[cpv], replaced=[cpv], changed=[index.package(cpv).cp]
        )
        assert answer["reachable"] == sorted(index.reachable([cpv], set(kinds))), cpv
        assert answer["affected"] == sorted(
            neighborhood.affected_cpvs(index, [index.package(cpv).cp], [cpv], [])
        ), cpv


def test_dynamic_deps_oracle_reads_what_emerge_reads_on_the_live_system(live_vardb):
    from _emerge.actions import load_emerge_config
    from _emerge.FakeVartree import FakeVartree

    from egraph_build.model import DEP_KINDS

    root_config = load_emerge_config().target_config
    fake = FakeVartree(root_config, dynamic_deps=True)
    fake.sync()
    portdb = root_config.trees["porttree"].dbapi
    vardb = root_config.trees["vartree"].dbapi
    differing = []
    for cpv in oracle.installed(vardb):
        _, strings, _ = oracle.dynamic_dep_strings(vardb, portdb, cpv)
        if strings != dict(zip(DEP_KINDS, fake.dbapi.aux_get(cpv, list(DEP_KINDS)))):
            differing.append(cpv)
    assert differing == []


@pytest.fixture(scope="module")
def live_databases():
    return cli.open_databases("/", "/")


@pytest.fixture(scope="module")
def live_evaluated(live_databases):
    return evaluated.build(*live_databases)


def test_evaluated_dependencies_agree_with_the_oracle(live_databases, live_evaluated):
    vardb, portdb = live_databases
    cpvs = list(oracle.installed(vardb))
    assert live_evaluated.installed() == tuple(cpvs)
    for cpv in random.Random(0).sample(cpvs, min(SAMPLE * 5, len(cpvs))):
        assert live_evaluated.deps(cpv) == oracle.dynamic_deps(vardb, portdb, cpv), cpv


def test_evaluated_candidates_agree_with_the_oracle(live_databases, live_evaluated):
    from portage import best
    from portage.versions import cpv_getkey

    vardb, portdb = live_databases
    installed_cpvs = set(oracle.installed(vardb))
    for cp in sorted({cpv_getkey(cpv) for cpv in installed_cpvs}):
        candidates = live_evaluated.candidates(cp)
        visible = [c for c in candidates if not c.reasons]
        assert {c.cpv for c in visible} == set(portdb.xmatch("match-visible", cp)), cp
        by_slot = {}
        for c in visible:
            by_slot.setdefault(c.slot, []).append(c.cpv)
        best_found = {slot: best(found) for slot, found in by_slot.items()}
        assert best_found == oracle.best_visible(portdb, cp), cp
        assert all(c.cpv in installed_cpvs for c in candidates if c.reasons), cp
    rng = random.Random(0)
    for c in rng.sample(live_evaluated.candidates(), SAMPLE):
        (default_repo,) = portdb.aux_get(c.cpv, ["repository"])
        if c.repo == default_repo:
            assert c.use == oracle.effective_use(portdb, c.cpv), c
        assert c.reasons == oracle.mask_reasons(portdb, c.cpv, c.repo), c


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_reads_the_live_evaluated_store(live_layer, live_evaluated, tmp_path):
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(live_layer, store.Meta("0", "0", "/", 0)))
    meta = store.EvaluatedMeta("0", "0", "/", 0, 0)
    store.write(
        store.evaluated_path(path), store.encode_evaluated(live_evaluated, meta)
    )
    exported = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(path), "--no-refresh", "export"]
        + ["--format", "json", "--evaluated"],
        capture_output=True,
        check=True,
    ).stdout
    assert exported == evaluated.to_json(live_evaluated).encode()
