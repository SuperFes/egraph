"""egraph against portage on this machine's own /var/db/pkg, read-only."""

import json
import os
import random
import subprocess

import pytest

from compare import QUERIES, assert_agrees
from egraph_build import cli, evaluated, installed, ledger, oracle, store

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


def test_the_ledger_reads_every_live_source_line_by_line(live_vardb):
    """No source of the live configuration falls back to portage's values at line 0."""
    from test_ledger import entries_of

    use_ledger = ledger.read(live_vardb.settings)
    assert [e for e in entries_of(use_ledger) if e.file and not e.line] == []


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
def live_databases():
    return cli.open_databases("/", "/")


@pytest.fixture(scope="module")
def live_evaluated(live_databases):
    return evaluated.build(*live_databases)


@pytest.fixture(scope="module")
def live_store(live_vardb, live_layer, live_evaluated, tmp_path_factory):
    from egraph_build.profile import implicit_iuse

    path = tmp_path_factory.mktemp("live") / "installed.egraph"
    meta = store.Meta("0", "0", "/", 0, implicit_iuse(live_vardb.settings))
    store.write(path, store.encode(live_layer, meta))
    evaluated_meta = store.EvaluatedMeta("0", "0", "/", 0, 0)
    store.write(
        store.evaluated_path(path),
        store.encode_evaluated(live_evaluated, evaluated_meta),
    )
    return path


def live_portdb(dynamic_deps, live_databases):
    return live_databases[1] if dynamic_deps else None


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
def test_cpp_queries_agree_with_portage(
    live_vardb, live_databases, live_store, dynamic_deps
):
    from conftest import dynamic_option
    from test_queries import parse_edges

    portdb = live_portdb(dynamic_deps, live_databases)
    option = dynamic_option(dynamic_deps)
    for (cpv,) in subjects_sample(live_vardb, "deps"):
        found = parse_edges(_egraph(live_store, "deps", *option, cpv))
        assert found == oracle.deps(live_vardb, cpv, portdb=portdb), cpv
        found = parse_edges(_egraph(live_store, "rdeps", *option, cpv))
        assert found == oracle.rdeps(live_vardb, cpv, portdb=portdb), cpv


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_broken_agrees_with_portage(
    live_vardb, live_databases, live_store, dynamic_deps
):
    from conftest import dynamic_option

    portdb = live_portdb(dynamic_deps, live_databases)
    expected = sorted("\t".join(item) for item in oracle.broken(live_vardb, portdb))
    found = _egraph(live_store, "broken", *dynamic_option(dynamic_deps))
    assert found.splitlines() == expected


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
    """depclean's answers by (with_bdeps, dynamic_deps)."""
    from depclean import depclean

    config = live_emerge_config
    return {
        (with_bdeps, dynamic_deps): depclean(
            config.trees, config.target_config.root, with_bdeps, dynamic_deps
        )
        for with_bdeps in (True, False)
        for dynamic_deps in (True, False)
    }


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_orphans_are_what_depclean_removes(
    live_store, live_depclean, with_bdeps, dynamic_deps
):
    from conftest import dynamic_option

    expected = live_depclean[with_bdeps, dynamic_deps]
    result = subprocess.run(
        [
            os.environ["EGRAPH"],
            "--store",
            str(live_store),
            "--no-refresh",
            "orphans",
            "--with-bdeps",
            "y" if with_bdeps else "n",
            *dynamic_option(dynamic_deps),
        ],
        capture_output=True,
        text=True,
    )
    assert (result.returncode != 0) == (expected.returncode != 0), result.stderr
    assert tuple(result.stdout.splitlines()) == expected.orphans


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_why_explains_every_kept_package(
    live_vardb, live_store, live_depclean, dynamic_deps
):
    from conftest import dynamic_option
    from test_why import assert_explains, why

    expected = live_depclean[True, dynamic_deps]
    for cpv in sorted(expected.kept):
        result = why(live_store, cpv, *dynamic_option(dynamic_deps))
        assert result.returncode == 0, result.stderr
        assert_explains(result.stdout, cpv, expected, live_vardb)


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_affected_agrees_with_the_fork_on_the_live_vdb(
    live_vardb, live_store, live_emerge_config, dynamic_deps
):
    """Against the fork's index over the vartree its depgraph uses: emerge's FakeVartree, which
    reads dependencies from the ebuilds under --dynamic-deps=y."""
    from _emerge.FakeVartree import FakeVartree

    from conftest import dynamic_option

    graph = pytest.importorskip("portage.dbapi._InstalledGraph")
    neighborhood = pytest.importorskip("_emerge.resolver.neighborhood")

    if dynamic_deps:
        fake = FakeVartree(live_emerge_config.target_config, dynamic_deps=True)
        fake.sync()
        vardb = fake.dbapi
    else:
        vardb = live_vardb
    index = graph.InstalledGraph.from_vardb(vardb)

    def ask(**request):
        result = subprocess.run(
            [os.environ["EGRAPH"], "--store", str(live_store), "--no-refresh"]
            + ["affected", *dynamic_option(dynamic_deps)],
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


def test_evaluated_dependencies_agree_with_the_oracle(live_databases, live_evaluated):
    vardb, portdb = live_databases
    cpvs = list(oracle.installed(vardb))
    assert live_evaluated.installed() == tuple(cpvs)
    for cpv in random.Random(0).sample(cpvs, min(SAMPLE * 5, len(cpvs))):
        assert live_evaluated.deps(cpv) == oracle.dynamic_deps(vardb, portdb, cpv), cpv


def test_evaluated_candidates_agree_with_the_oracle(live_databases, live_evaluated):
    import portage
    import update
    from portage import best
    from portage.versions import cpv_getkey

    vardb, portdb = live_databases
    installed_cpvs = set(oracle.installed(vardb))

    def invalid(cpv):
        return update.invalid_reasons(portage.db, "/", cpv, cpv.repo)

    for cp in sorted({cpv_getkey(cpv) for cpv in installed_cpvs}):
        candidates = live_evaluated.candidates(cp)
        visible = [c for c in candidates if not c.reasons]
        assert {c.cpv for c in visible} == {
            cpv for cpv in portdb.xmatch("match-visible", cp) if not invalid(cpv)
        }, cp
        by_slot = {}
        for c in visible:
            by_slot.setdefault(c.slot, []).append(c.cpv)
        best_found = {slot: best(found) for slot, found in by_slot.items()}
        assert best_found == oracle.best_visible(portdb, cp, invalid), cp
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


def test_possible_dependencies_agree_with_the_oracle(live_databases, live_evaluated):
    """Single toggles only for completeness: the live IUSE makes pairs too many."""
    from compare import possible_mismatches
    from test_evaluated import layer_possible

    vardb, portdb = live_databases
    cpvs = [pkg.cpv for pkg in live_evaluated if pkg.source == evaluated.EBUILD]
    for cpv in random.Random(0).sample(cpvs, min(SAMPLE, len(cpvs))):
        found = layer_possible(live_evaluated, cpv)
        deps = oracle.dynamic_deps(vardb, portdb, cpv)
        assert possible_mismatches(vardb, portdb, cpv, found, deps, size=1) == [], cpv


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_possible_dependencies_are_the_evaluated_layers(live_evaluated, live_store):
    from test_evaluated import layer_possible
    from test_queries import parse_possible

    every = set()
    for cpv in live_evaluated.installed():
        every |= layer_possible(live_evaluated, cpv)
    for cpv in random.Random(1).sample(live_evaluated.installed(), SAMPLE):
        _, possible = parse_possible(_egraph(live_store, "deps", "--possible", cpv))
        assert possible == {(e, f) for e, f in every if e.parent == cpv}, cpv
        _, possible = parse_possible(_egraph(live_store, "rdeps", "--possible", cpv))
        assert possible == {(e, f) for e, f in every if e.child == cpv}, cpv


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_cpp_matches_ebuilds_as_depgraph_on_every_live_atom(
    live_emerge_config, live_layer, live_evaluated, live_store
):
    from conftest import System
    from test_match import assert_ebuilds_match, tree_atoms

    root = live_emerge_config.target_config.root
    trees = live_emerge_config.trees
    system = System(root, trees[root]["vartree"].dbapi, trees)
    atoms = sorted(set(tree_atoms(live_layer)) | set(tree_atoms(live_evaluated)))
    assert_ebuilds_match(system, live_store, atoms, live_evaluated)


@pytest.fixture(scope="module")
def live_updates(live_emerge_config):
    """emerge -pu @installed's answers by (newuse, changed_use)."""
    from test_evaluated import USE_MODES
    from update import updates

    config = live_emerge_config
    return {
        mode: updates(config.trees, config.target_config.root, *mode)
        for mode in USE_MODES
    }


@pytest.mark.parametrize(
    "newuse, changed_use",
    [(False, False), (True, False), (False, True)],
    ids=["update", "newuse", "changed-use"],
)
def test_updates_cover_emerges(live_evaluated, live_updates, newuse, changed_use):
    """Everything emerge replaces, as egraph names it. egraph also lists what the resolver holds
    back for an installed dependent's bound (docs/findings.md)."""
    from compare import layer_updates

    expected = live_updates[newuse, changed_use].replaced
    found = layer_updates(live_evaluated, newuse, changed_use)
    assert {cpv: found.get(cpv) for cpv in expected} == expected


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
@pytest.mark.parametrize(
    "option, newuse, changed_use",
    [(None, False, False), ("--newuse", True, False), ("--changed-use", False, True)],
    ids=["update", "newuse", "changed-use"],
)
@pytest.mark.parametrize("deep", [False, True], ids=["u", "uD"])
def test_world_updates_are_emerges(
    live_emerge_config, live_store, option, newuse, changed_use, deep
):
    """updates --world merges what emerge -pu @world does, and with -D what -puD does."""
    from test_queries import merged
    from update import updates

    config = live_emerge_config
    expected = updates(
        config.trees,
        config.target_config.root,
        newuse,
        changed_use,
        deep=deep,
        target="@world",
    )
    assert expected.success
    options = [*filter(None, [option]), *(["-D"] if deep else [])]
    output = _egraph(live_store, "updates", "--world", *options)
    assert merged(output) == (expected.replaced, expected.rebuilt, expected.new)


def test_visible_is_emerges(live_emerge_config, live_evaluated):
    from update import equiv_visible

    config = live_emerge_config
    expected = equiv_visible(config.trees, config.target_config.root)
    assert {pkg.cpv: pkg.visible for pkg in live_evaluated} == expected


@pytest.mark.parametrize("dynamic", [True, False], ids=["dynamic-deps", "vdb-deps"])
def test_masked_is_emerges(live_emerge_config, live_evaluated, dynamic):
    from update import masked

    config = live_emerge_config
    expected = masked(config.trees, config.target_config.root, dynamic)
    from test_evaluated import weighed

    found = {
        pkg.cpv: pkg.masked if dynamic else pkg.vdb_masked for pkg in live_evaluated
    }
    kept = weighed(live_evaluated)
    assert found == {cpv: masked and cpv in kept for cpv, masked in expected.items()}


def test_every_repository_required_use_checks_as_portage_does(live_databases):
    """Each distinct REQUIRED_USE of the live repositories, under sampled USE of its flags."""
    import portage
    from portage.dep import get_required_use_flags
    from portage.eapi import eapi_has_required_use

    import test_required_use

    if not test_required_use.SHADOW:
        pytest.skip("set EGRAPH_SHADOW to the shadow binary (meson test does)")
    _, portdb = live_databases
    strings = set()
    for cp in portdb.cp_all():
        for cpv in portdb.cp_list(cp):
            try:
                required, eapi = portdb.aux_get(cpv, ["REQUIRED_USE", "EAPI"])
            except KeyError:
                continue
            if required and eapi_has_required_use(eapi):
                strings.add((" ".join(required.split()), eapi))
    rng = random.Random(16)
    cases = []
    for required, eapi in sorted(strings):
        try:
            flags = sorted(get_required_use_flags(required, eapi))
        except portage.exception.InvalidDependString:
            continue
        for _ in range(16):
            use = frozenset(flag for flag in flags if rng.random() < 0.5)
            cases.append((required, use, eapi))
    assert cases
    empty_true = portage.eapi.eapi_empty_groups_always_true
    ours = test_required_use.shadow(
        (required, use, empty_true(eapi)) for required, use, eapi in cases
    )
    wrong = [
        (case, answer)
        for case, answer in zip(cases, ours)
        if answer != test_required_use.portage_answer(*case)
    ]
    assert not wrong


def test_every_repository_dependency_string_reduces_as_portage_does(live_databases):
    """Each distinct dependency string of the live repositories, under sampled USE of its
    ebuild's IUSE."""
    import portage
    from portage.eapi import eapi_empty_groups_always_true

    import test_use_reduce

    if not test_use_reduce.SHADOW:
        pytest.skip("set EGRAPH_SHADOW to the shadow binary (meson test does)")
    _, portdb = live_databases
    keys = ["EAPI", "IUSE", *evaluated.DEP_KINDS]
    strings = {}
    for cp in portdb.cp_all():
        for cpv in portdb.cp_list(cp):
            try:
                metadata = dict(zip(keys, portdb.aux_get(cpv, keys)))
            except KeyError:
                continue
            iuse = frozenset(flag.lstrip("+-") for flag in metadata["IUSE"].split())
            for kind in evaluated.DEP_KINDS:
                if "?" in metadata[kind]:
                    strings.setdefault((metadata[kind], metadata["EAPI"]), iuse)
    rng = random.Random(16)
    cases = []
    for (string, eapi), iuse in sorted(strings.items(), key=lambda item: item[0]):
        try:
            test_use_reduce._one(string, frozenset(), eapi)
        except portage.exception.PortageException:
            continue
        for _ in range(4):
            use = frozenset(flag for flag in iuse if rng.random() < 0.5)
            cases.append(
                (tuple(string.split()), use, eapi, eapi_empty_groups_always_true(eapi))
            )
    assert cases
    assert not test_use_reduce.disagreements(cases)


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
def test_every_repository_version_is_visible_or_masked_as_portage_has_it(
    live_databases, live_layer, tmp_path
):
    from test_visibility import portage_view

    from egraph_build import repository

    _, db = live_databases
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(live_layer, store.Meta("0", "0", "/", 0)))
    index = repository.read(db)
    meta = store.RepositoryMeta("0", "0", "/", 0)
    store.write(store.repository_path(path), store.encode_repository(index, meta))
    lines = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(path), "--no-refresh", "versions"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    ours = {
        fields[0]: (fields[2] == "visible", tuple(fields[3:]))
        for fields in (line.split("\t") for line in lines)
    }
    assert ours == portage_view(db)


@pytest.mark.skipif(
    not os.environ.get("EGRAPH"), reason="set EGRAPH to the egraph binary"
)
@pytest.mark.parametrize("searchdesc", [False, True], ids=["names", "descriptions"])
def test_search_finds_and_shows_what_emerge_does_on_the_live_system(
    live_databases, live_layer, tmp_path, searchdesc
):
    from collections import namedtuple

    from _emerge.actions import load_emerge_config
    from test_search import emerge_lines

    from egraph_build import repository

    vardb, db = live_databases
    path = tmp_path / "installed.egraph"
    store.write(path, store.encode(live_layer, store.Meta("0", "0", "/", 0)))
    layer = evaluated.build(vardb, db)
    meta = store.EvaluatedMeta("0", "0", "/", 0, 0)
    store.write(store.evaluated_path(path), store.encode_evaluated(layer, meta))
    index_meta = store.RepositoryMeta("0", "0", "/", 0)
    index = repository.read(db)
    store.write(store.repository_path(path), store.encode_repository(index, index_meta))
    keys = ["openssl", "opnessl", "%^dev-libs/lib", "python$", "@sys-apps", "qt"]
    if searchdesc:
        keys = ["toolkit", "%^a ", "library"]
    config = load_emerge_config()
    eroot = config.target_config.root
    System = namedtuple("System", "eroot vardb trees")
    system = System(eroot, config.trees[eroot]["vartree"].dbapi, config.trees)
    options = ["-S"] if searchdesc else []
    ours = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(path), "--no-refresh", "search"]
        + options
        + ["--", *keys],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    assert ours == emerge_lines(system, keys, searchdesc)
