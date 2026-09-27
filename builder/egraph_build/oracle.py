"""Portage's own answers to egraph's queries, computed the slow way.

Nothing here indexes or caches: every answer comes straight from vardbapi
matching, which honors slots and USE dependencies with their defaults, and
from use_reduce over each package's installed USE. This is the reference the
installed layer, and through it the store, is compared against, so it must
stay a thin layer over portage rather than grow semantics of its own.
"""

from portage.dep import Atom, use_reduce
from portage.dep.soname.parse import parse_soname_deps
from portage.exception import InvalidAtom, InvalidData, InvalidDependString

from egraph_build.model import DEP_KINDS, Edge, SonameUse


def installed(vardb):
    return tuple(sorted(str(cpv) for cpv in vardb.cpv_all()))


def matches(vardb, atom):
    """Installed cpvs satisfying a dependency atom."""
    return tuple(sorted(str(cpv) for cpv in vardb.match(atom)))


def _flatten(tokens, choice, out):
    tokens = iter(tokens)
    for token in tokens:
        if token == "||":
            _flatten(next(tokens), True, out)
        elif isinstance(token, list):
            _flatten(token, choice, out)
        else:
            out.append((token, choice))


def dep_atoms(vardb, cpv, kind):
    """(Atom, choice) for every atom left in one dependency kind after USE reduction.

    Raises InvalidDependString or InvalidAtom when portage cannot parse it.
    """
    depstring, use, eapi = vardb.aux_get(cpv, [kind, "USE", "EAPI"])
    tokens = use_reduce(
        depstring,
        uselist=frozenset(use.split()),
        eapi=eapi or None,
        opconvert=False,
        token_class=Atom,
    )
    out = []
    _flatten(tokens, False, out)
    return out


def errors(vardb):
    """(cpv, key) of every dependency or soname string portage cannot parse."""
    found = set()
    for cpv in installed(vardb):
        for kind in DEP_KINDS:
            try:
                dep_atoms(vardb, cpv, kind)
            except (InvalidAtom, InvalidDependString):
                found.add((cpv, kind))
        for key in ("PROVIDES", "REQUIRES"):
            (value,) = vardb.aux_get(cpv, [key])
            try:
                tuple(parse_soname_deps(value))
            except InvalidData:
                found.add((cpv, key))
    return frozenset(found)


def deps(vardb, cpv, kinds=DEP_KINDS):
    """Edges from cpv to the installed packages its atoms match.

    Blockers are constraints rather than dependencies and yield no edges. A
    kind portage cannot parse yields none either; errors() reports it.
    """
    edges = set()
    for kind in kinds:
        try:
            atoms = dep_atoms(vardb, cpv, kind)
        except (InvalidAtom, InvalidDependString):
            continue
        for atom, choice in atoms:
            if atom.blocker:
                continue
            for child in matches(vardb, atom):
                edges.add(Edge(cpv, child, kind, str(atom), choice))
    return frozenset(edges)


def rdeps(vardb, cpv, kinds=DEP_KINDS):
    """Edges into cpv, found by evaluating every installed package."""
    return frozenset(
        edge
        for parent in installed(vardb)
        for edge in deps(vardb, parent, kinds)
        if edge.child == cpv
    )


def sonames(vardb, cpv, key):
    (value,) = vardb.aux_get(cpv, [key])
    try:
        return tuple(parse_soname_deps(value))
    except InvalidData:
        return ()


def soname_providers(vardb, soname):
    return _soname_users(vardb, soname, "PROVIDES")


def soname_consumers(vardb, soname):
    return _soname_users(vardb, soname, "REQUIRES")


def _soname_users(vardb, soname, key):
    return frozenset(
        SonameUse(cpv, atom.multilib_category)
        for cpv in installed(vardb)
        for atom in sonames(vardb, cpv, key)
        if atom.soname == soname
    )
