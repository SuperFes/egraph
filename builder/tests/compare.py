"""Side-by-side runs of a query through portage (the oracle) and through egraph."""

import random

from portage.exception import InvalidAtom, InvalidDependString

from egraph_build import oracle
from egraph_build.model import DEP_KINDS

QUERIES = (
    "installed",
    "errors",
    "matches",
    "deps",
    "rdeps",
    "soname_providers",
    "soname_consumers",
)


def _atoms(vardb):
    found = set()
    for cpv in oracle.installed(vardb):
        for kind in DEP_KINDS:
            try:
                atoms = oracle.dep_atoms(vardb, cpv, kind)
            except (InvalidAtom, InvalidDependString):
                continue
            found.update(str(atom) for atom, _ in atoms if not atom.blocker)
    return sorted(found)


def _sonames(vardb):
    found = set()
    for cpv in oracle.installed(vardb):
        for key in ("PROVIDES", "REQUIRES"):
            found.update(atom.soname for atom in oracle.sonames(vardb, cpv, key))
    return sorted(found)


def subjects(vardb, query):
    """Argument tuples covering every package, atom or soname the query can be asked about."""
    if query in ("installed", "errors"):
        return [()]
    if query in ("deps", "rdeps"):
        return [(cpv,) for cpv in oracle.installed(vardb)]
    if query == "matches":
        return [(atom,) for atom in _atoms(vardb)]
    if query.startswith("soname_"):
        return [(soname,) for soname in _sonames(vardb)]
    raise ValueError(query)


def mismatches(vardb, layer, query, sample=None):
    """Every subject where egraph's answer differs from portage's, as readable lines.

    sample limits the check to that many subjects, chosen reproducibly.
    """
    chosen = subjects(vardb, query)
    if sample is not None and len(chosen) > sample:
        chosen = sorted(random.Random(query).sample(chosen, sample))
    lines = []
    for args in chosen:
        want = getattr(oracle, query)(vardb, *args)
        got = getattr(layer, query)(*args)
        if got != want:
            call = f"{query}({', '.join(map(repr, args))})"
            lines.append(f"{call}\n  portage: {want!r}\n  egraph:  {got!r}")
    return lines


def assert_agrees(vardb, layer, query, sample=None):
    lines = mismatches(vardb, layer, query, sample)
    assert not lines, "egraph disagrees with portage:\n" + "\n".join(lines)
