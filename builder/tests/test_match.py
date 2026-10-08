"""egraph's atom matching in shadow against portage's vardb.match."""

import os
import subprocess

import pytest
from portage.dep import Atom
from portage.exception import InvalidAtom

from conftest import portdb, write_stores
from egraph_build import evaluated, installed, oracle, store
from egraph_build.profile import implicit_iuse
from scenarios import ATOM_VERSIONS

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

EXTRA_VERSIONS = ("1.0.1", "0", "01", "1.0-r0", "1.0-r5", "3", "1.0_p", "1.0_p1-r1")
FLAGS = (
    "a",
    "b",
    "c",
    "prefix",
    "x86",
    "amd64",
    "elibc_glibc",
    "elibc_musl",
    "kernel_linux",
    "stray",
)


def corpus():
    atoms = ["dev-libs/v", "dev-libs/nothing"]
    for version in ATOM_VERSIONS + EXTRA_VERSIONS:
        for op in ("<", "<=", "=", "~", ">=", ">"):
            atoms.append(f"{op}dev-libs/v-{version}")
        atoms.append(f"=dev-libs/v-{version}*")
    atoms += [
        "=dev-libs/v-1.0.0*",
        "=dev-libs/v-2*",
        "=dev-libs/v-20*",
        "=dev-libs/v-1.0_p1-r1*",
        "dev-libs/v:3",
        "dev-libs/v:3/3",
        "dev-libs/v:3/4",
        "app-misc/s:1",
        "app-misc/s:1/1.5",
        "app-misc/s:1/1.4",
        "app-misc/s:2=",
        "app-misc/s:2/2.0=",
        "app-misc/s:*",
        "app-misc/s:=",
        "app-misc/s::test_repo",
        "app-misc/s::other",
        ">=app-misc/s-2:2::test_repo",
    ]
    for pkg in ("app-misc/u4", "app-misc/u8"):
        for flag in FLAGS:
            for spelling in ("{}", "-{}", "{}(+)", "{}(-)", "-{}(+)", "-{}(-)"):
                atoms.append(f"{pkg}[{spelling.format(flag)}]")
        atoms += [f"{pkg}[a,-c(-)]", f"{pkg}[a,c(+),-b]"]
    # Rejected by portage as well.
    atoms += [
        "dev-libs/v-1.0",
        "dev-libs/v-1",
        ">=dev-libs/v",
        "=dev-libs/v-1*-r1",
        "dev-libs/v[]",
        "dev-libs/v:",
        "dev-libs/v-1a",
    ]
    return atoms


def valid(text):
    try:
        Atom(text, allow_repo=True)
    except InvalidAtom:
        return False
    return True


def egraph_matches(path, atoms, *options):
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "match", *options, *atoms],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    found = {atom: set() for atom in atoms}
    for line in result.stdout.splitlines():
        atom, cpv = line.split("\t")
        found[atom].add(cpv)
    return found


def write_store(vardb, path):
    meta = store.Meta("0", "0", "/", 0, implicit_iuse(vardb.settings))
    store.write(path, store.encode(installed.build(vardb), meta))


def test_corpus_matches_as_portage_does(playgrounds, tmp_path):
    vardb = playgrounds("atoms").vardb
    path = tmp_path / "installed.egraph"
    write_store(vardb, path)
    atoms = [atom for atom in corpus() if valid(atom)]
    assert len(atoms) > 300
    found = egraph_matches(path, atoms)
    wrong = [
        f"{atom}: portage {sorted(oracle.matches(vardb, atom))}, egraph {sorted(found[atom])}"
        for atom in atoms
        if found[atom] != set(oracle.matches(vardb, atom))
    ]
    assert not wrong, "\n".join(wrong)


def test_invalid_atoms_are_rejected(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_store(playgrounds("atoms").vardb, path)
    rejected = [atom for atom in corpus() if not valid(atom)]
    assert rejected
    for atom in rejected:
        result = subprocess.run(
            [EGRAPH, "--store", str(path), "--no-refresh", "match", atom],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 1, atom
        assert result.stderr.startswith(f"egraph: {atom}: invalid atom: "), atom


def tree_atoms(layer):
    """Every atom in the dependency trees, blockers as the atom they block."""
    return sorted(
        {
            node.atom.lstrip("!")
            for pkg in layer
            for nodes in pkg.deps
            for node in nodes
            if node.atom
        }
    )


def test_every_tree_atom_matches_as_portage_does(scenario, tmp_path):
    vardb = scenario.vardb
    path = tmp_path / "installed.egraph"
    write_store(vardb, path)
    atoms = tree_atoms(installed.build(vardb))
    if not atoms:
        pytest.skip("no dependency atoms in this scenario")
    found = egraph_matches(path, atoms)
    for atom in atoms:
        assert found[atom] == set(oracle.matches(vardb, atom)), atom


def assert_ebuilds_match(system, path, atoms, layer=None):
    """As depgraph. layer is the evaluated layer at path, built when omitted."""
    import update

    layer = layer or evaluated.build(system.vardb, portdb(system))
    candidates = list(layer.candidates())
    found = egraph_matches(path, atoms, "--candidates")
    wrong = [
        f"{atom}: depgraph {sorted(expected)}, egraph {sorted(found[atom])}"
        for atom in atoms
        for expected in [
            update.candidate_matches(system.trees, system.eroot, atom, candidates)
        ]
        if found[atom] != expected
    ]
    assert not wrong, "\n".join(wrong)


def test_corpus_matches_ebuilds_as_depgraph_does(playgrounds, tmp_path):
    system = playgrounds("atoms")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    assert_ebuilds_match(system, path, [atom for atom in corpus() if valid(atom)])


def test_every_tree_atom_matches_ebuilds_as_depgraph_does(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    layer = evaluated.build(scenario.vardb, portdb(scenario))
    atoms = sorted(
        set(tree_atoms(installed.build(scenario.vardb))) | set(tree_atoms(layer))
    )
    if not atoms:
        pytest.skip("no dependency atoms in this scenario")
    assert_ebuilds_match(scenario, path, atoms)


CONFIG_ATOMS = (
    "*/*",
    "dev-libs/*",
    "*/v",
    "dev-*/*",
    "*-misc/*",
    "*/*:3",
    "*/*:1/1.5",
    "*/*::test_repo",
    "*/*::other",
    "app-misc/*:1",
    "=dev-libs/v-*1*",
    "=dev-libs/v-*r1*",
    "=dev-libs/v-*_p*",
    "=*/*-*0*",
)


def egraph_config_matches(path, atoms):
    """{cpv::repo: the atoms that match it, in the order egraph applies them}."""
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "match", "--config", *atoms],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    found = {}
    for line in result.stdout.splitlines():
        atom, pkg = line.split("\t")
        found.setdefault(pkg, []).append(atom)
    return found


def test_config_atoms_match_in_portages_order(playgrounds, tmp_path):
    """Configuration atoms, plain and extended, against every ebuild: those match_from_list
    matches, in the order ordered_by_atom_specificity applies them."""
    from portage.package.ebuild._config.helper import ordered_by_atom_specificity
    from portage.versions import _pkg_str

    system = playgrounds("atoms")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    plain = [atom for atom in corpus() if valid(atom) and "[" not in atom]
    atoms = sorted(set(plain) | set(CONFIG_ATOMS))
    parsed = {Atom(atom, allow_wildcard=True, allow_repo=True): atom for atom in atoms}
    found = egraph_config_matches(path, atoms)
    assert max(len(matched) for matched in found.values()) > 50
    layer = evaluated.build(system.vardb, portdb(system))
    wrong = []
    for c in layer.candidates():
        pkg = _pkg_str(c.cpv, slot=f"{c.slot}/{c.sub_slot}", repo=c.repo)
        expected = ordered_by_atom_specificity(parsed, pkg)
        key = f"{c.cpv}::{c.repo}"
        if found.get(key, []) != expected:
            wrong.append(f"{key}: portage {expected}, egraph {found.get(key, [])}")
    assert not wrong, "\n".join(wrong)
