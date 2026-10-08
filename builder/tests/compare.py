"""Side-by-side runs of a query through portage (the oracle) and through egraph."""

import itertools
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


def possible_mismatches(vardb, portdb, cpv, found, deps, size=None):
    """Where found, cpv's possible edges as (Edge, flags) pairs, disagrees with the oracle.

    Each edge must be one its flags add (oracle.possible), and no fewer of them may add it.
    Every edge the oracle adds for up to size toggles (any number when None) must be in found
    with no more flags, or be one of deps, the edges cpv has already. That side compares
    children only: toggles beyond the ones a conditional needs can change its atom's USE
    dependencies, and so its string.
    """
    problems = []
    for edge, flags in sorted(found):
        if edge not in oracle.possible(vardb, portdb, cpv, flags):
            problems.append(f"{edge} with {flags}: the flags do not add it")
        for fewer in range(1, len(flags)):
            for chosen in itertools.combinations(flags, fewer):
                if edge in oracle.possible(vardb, portdb, cpv, chosen):
                    problems.append(f"{edge} with {flags}: {chosen} add it already")
    source, strings, _ = oracle.dynamic_dep_strings(vardb, portdb, cpv)
    toggles = oracle.use_toggles(vardb, portdb, cpv) if source == "ebuild" else ()
    # Only flags a conditional names change what use_reduce selects; the rest only multiply
    # the combinations.
    conditional = {
        token.rstrip("?").lstrip("!")
        for string in strings.values()
        for token in string.split()
        if token.endswith("?")
    }
    toggles = [toggle for toggle in toggles if toggle.lstrip("-") in conditional]
    have = {(edge.kind, edge.child) for edge in deps}
    for count in range(1, (size or len(toggles)) + 1):
        for chosen in itertools.combinations(toggles, count):
            for edge in oracle.possible(vardb, portdb, cpv, chosen):
                covered = (edge.kind, edge.child) in have or any(
                    (other.kind, other.child) == (edge.kind, edge.child)
                    and set(flags) <= set(chosen)
                    for other, flags in found
                )
                if not covered:
                    problems.append(f"{edge} with {chosen}: missing")
    return problems


def rebuild_flag(flag):
    """A rebuild flag's name, without emerge's markers."""
    return flag.strip("()").lstrip("-").rstrip("%*")


def layer_updates(layer, newuse=False, changed_use=False):
    """{installed cpv: update.Replacement} as the layer answers emerge -u @installed, with -N
    or -U."""
    from update import Replacement

    found = {}
    for pkg in layer:
        if pkg.target is None:
            continue
        cpv, repo = pkg.target
        if not pkg.rebuild:
            found[pkg.cpv] = Replacement(cpv, repo)
            continue
        flags = frozenset(
            rebuild_flag(flag)
            for flag in pkg.rebuild
            if newuse or (changed_use and flag.rstrip(")").endswith("*"))
        )
        if flags:
            found[pkg.cpv] = Replacement(cpv, repo, flags)
    return found
