"""What each merge waits for, held to the scheduler graph emerge would run egraph's plan by."""

import json
import os
import re

import pytest

from portage.dep import Atom, use_reduce
from portage.versions import cpv_getkey

from conftest import portdb, write_stores
from egraph_build.cli import EXIT_REFUSED
from resume import same_version, scheduled
from scenarios import SCENARIOS
from test_queries import PLAN_MODES, egraph, plan_requests
from test_resume import UPDATE_MODES

pytestmark = pytest.mark.skipif(
    not os.environ.get("EGRAPH"),
    reason="set EGRAPH to the egraph binary (meson test does)",
)

# The dependency kinds behind each of egraph's wait letters.
KINDS = {
    "b": ("DEPEND", "BDEPEND"),
    "i": ("IDEPEND",),
    "r": ("RDEPEND",),
    "p": ("PDEPEND",),
}


def table(text):
    """(merge cpvs in egraph's order, {(cpv, cpv it waits for): letters}, {uninstalled cpv:
    merge cpvs it waits for}) from updates -t or plan -t output."""
    lines = [line.split("\t") for line in text.splitlines()]
    rows = [fields for fields in lines if fields[0]]
    order = [fields[4] for fields in rows]
    waits = {}
    for fields in rows:
        for wait in fields[1].split():
            place, kinds = re.fullmatch(r"(\d+)([a-z]*)", wait).groups()
            waits[(fields[4], order[int(place) - 1])] = kinds
    uninstalls = {
        fields[2]: frozenset(order[int(place) - 1] for place in fields[1].split())
        for fields in lines
        if len(fields) > 3 and not fields[0] and fields[3] == "uninstall"
    }
    return order, waits, uninstalls


def in_any_of(system, cpv, repo, use, kinds, waited):
    """Whether each of cpv's atoms of those kinds that waited matches is inside a || group."""
    db = portdb(system)
    eapi, *deps = db.aux_get(cpv, ["EAPI", *kinds], myrepo=repo)
    matching = set(db.xmatch("match-all", cpv_getkey(waited)))

    def matches(atom):
        return waited in db.xmatch("match-all", atom.without_use) and waited in matching

    def walk(tree, inside):
        # opconvert leads each || group's list with "||".
        inside = inside or (tree[:1] == ["||"])
        found = True
        for item in tree:
            if isinstance(item, list):
                found = walk(item, inside) and found
            elif item != "||" and not item.blocker and matches(item):
                found = found and inside
        return found

    return all(
        walk(
            use_reduce(dep, uselist=use, opconvert=True, token_class=Atom, eapi=eapi),
            False,
        )
        for dep in deps
    )


def check_waits(system, result, path, repos):
    """egraph's waits are the scheduler graph's: each direct edge with its kinds, but for the
    alternatives of a || emerge did not choose; each merge reached only through installed
    packages, among more; its order keeps each wait emerge's order does; and each uninstall
    waits for the merges emerge's does.
    """
    if result.returncode == EXIT_REFUSED or not path.exists():
        return
    entry = json.loads(path.read_text())
    path.unlink()
    order, waits, uninstalls = table(result.stdout)
    if not order:
        return
    found = scheduled(system.trees, system.eroot, entry)
    assert found.success
    spell = same_version(set(order))
    edges = {(spell(a), spell(b)): kinds for (a, b), kinds in found.edges.items()}
    through = {(spell(a), spell(b)) for a, b in found.through}
    use = {spell(cpv): flags for cpv, flags in found.use.items()}
    assert set(map(spell, found.order)) == set(order)
    for pair in set(edges) | set(waits):
        direct = set(waits.get(pair, "")) - {"t"}
        expected = set(edges.get(pair, ""))
        assert expected <= direct, (pair, waits.get(pair), edges.get(pair))
        for letter in direct - expected:
            assert letter in KINDS, (pair, waits.get(pair), edges.get(pair))
            assert in_any_of(
                system, pair[0], repos[pair[0]], use[pair[0]], KINDS[letter], pair[1]
            ), (pair, waits.get(pair), edges.get(pair))
    # egraph follows every installed package's dependencies, emerge only those its graph holds.
    assert through <= {pair for pair, kinds in waits.items() if "t" in kinds}
    place = {cpv: i for i, cpv in enumerate(order)}
    emerge_place = {spell(cpv): i for i, cpv in enumerate(found.order)}
    for (cpv, other), kinds in edges.items():
        if set(kinds) & set("birp") and emerge_place[other] < emerge_place[cpv]:
            assert place[other] < place[cpv], (cpv, other, kinds)
    assert uninstalls == {
        cpv: frozenset(map(spell, merges)) for cpv, merges in found.uninstalls.items()
    }


def repositories(text):
    return {
        fields[4]: fields[5]
        for fields in (line.split("\t") for line in text.splitlines())
        if fields[0]
    }


@pytest.mark.parametrize("mode", sorted(UPDATE_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_update_waits_are_emerges(playgrounds, tmp_path, name, mode):
    system = playgrounds(name)
    store = tmp_path / "installed.egraph"
    write_stores(system, store, eroot=system.eroot)
    path = tmp_path / "resume.json"
    result = egraph(
        store, "updates", "-t", *UPDATE_MODES[mode], "--resume-list", str(path)
    )
    check_waits(system, result, path, repositories(result.stdout))


@pytest.mark.parametrize("mode", sorted(PLAN_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_plan_waits_are_emerges(playgrounds, tmp_path, name, mode):
    system = playgrounds(name)
    store = tmp_path / "installed.egraph"
    write_stores(system, store, request_all=True, eroot=system.eroot)
    path = tmp_path / "resume.json"
    _, flags = PLAN_MODES[mode]
    for target in plan_requests(name):
        result = egraph(
            store, "plan", "-t", *flags, "--resume-list", str(path), target, check=False
        )
        if result.returncode == 1:
            continue
        assert result.returncode in (0, EXIT_REFUSED), (target, result.stderr)
        check_waits(system, result, path, repositories(result.stdout))
