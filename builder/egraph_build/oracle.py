"""Portage's own answers to egraph's queries, computed the slow way.

Nothing here indexes or caches: every answer comes straight from vardbapi
matching, which honors slots and USE dependencies with their defaults, and
from use_reduce over each package's installed USE. This is the reference the
installed layer, and through it the store, is compared against, so it must
stay a thin layer over portage rather than grow semantics of its own.
"""

from portage.dep import Atom, paren_enclose, use_reduce
from portage.dep.soname.parse import parse_soname_deps
from portage.exception import InvalidAtom, InvalidData, InvalidDependString

from egraph_build import dynamic
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


def _all_of(vardb, tokens):
    tokens = iter(tokens)
    for token in tokens:
        if token == "||":
            if not _any_of(vardb, next(tokens)):
                return False
        elif isinstance(token, list):
            if not _all_of(vardb, token):
                return False
        elif not token.blocker and not vardb.match(token):
            return False
    return True


def _any_of(vardb, alternatives):
    # An empty group only survives use_reduce in EAPIs where portage counts it as satisfied.
    if not alternatives:
        return True
    tokens = iter(alternatives)
    for token in tokens:
        if token == "||":
            if _any_of(vardb, next(tokens)):
                return True
        elif isinstance(token, list):
            if _all_of(vardb, token):
                return True
        elif token.blocker or vardb.match(token):
            return True
    return False


def _reduced(vardb, cpv, kind):
    depstring, use, eapi = vardb.aux_get(cpv, [kind, "USE", "EAPI"])
    return use_reduce(
        depstring,
        uselist=frozenset(use.split()),
        eapi=eapi or None,
        opconvert=False,
        token_class=Atom,
    )


def broken(vardb):
    """(cpv, kind, dependency) for each top-level dependency nothing installed satisfies.

    Evaluated straight from portage's reduced dependency lists, with the dependency rendered
    by paren_enclose. Blockers are constraints, not dependencies, and never count.
    """
    found = set()
    for cpv in installed(vardb):
        for kind in DEP_KINDS:
            try:
                tokens = iter(_reduced(vardb, cpv, kind))
            except (InvalidAtom, InvalidDependString):
                continue
            for token in tokens:
                if token == "||":
                    group = next(tokens)
                    if not _any_of(vardb, group):
                        found.add((cpv, kind, paren_enclose(["||", group])))
                elif isinstance(token, list):
                    if not _all_of(vardb, token):
                        found.add((cpv, kind, paren_enclose([token])))
                elif not token.blocker and not vardb.match(token):
                    found.add((cpv, kind, str(token)))
    return frozenset(found)


# The repository side: what emerge sees of an installed package through its ebuild, and of the
# versions its repositories offer. Each takes the ebuild repositories' portdbapi beside the vardb.


def dynamic_dep_strings(vardb, portdb, cpv):
    """(source, {kind: dependency string}, EAPI) as emerge's default --dynamic-deps=y sees cpv."""
    return dynamic.dependency_strings(vardb, portdb, cpv)


def dynamic_dep_atoms(vardb, portdb, cpv, kind):
    """dep_atoms() over the dynamic dependency strings, reduced under the installed USE."""
    _, strings, eapi = dynamic_dep_strings(vardb, portdb, cpv)
    (use,) = vardb.aux_get(cpv, ["USE"])
    tokens = use_reduce(
        strings[kind],
        uselist=frozenset(use.split()),
        eapi=eapi or None,
        opconvert=False,
        token_class=Atom,
    )
    out = []
    _flatten(tokens, False, out)
    return out


def dynamic_deps(vardb, portdb, cpv, kinds=DEP_KINDS):
    """deps() as emerge's default --dynamic-deps=y sees them."""
    edges = set()
    for kind in kinds:
        try:
            atoms = dynamic_dep_atoms(vardb, portdb, cpv, kind)
        except (InvalidAtom, InvalidDependString):
            continue
        for atom, choice in atoms:
            if atom.blocker:
                continue
            for child in matches(vardb, atom):
                edges.add(Edge(cpv, child, kind, str(atom), choice))
    return frozenset(edges)


def effective_use(portdb, cpv):
    """The flags cpv's ebuild would be built with now, within its IUSE."""
    from portage.package.ebuild.config import config

    settings = config(clone=portdb.settings)
    settings.setcpv(cpv, mydb=portdb)
    return tuple(sorted(settings["PORTAGE_USE"].split()))


def mask_reasons(portdb, cpv, repo=None):
    """Why cpv's ebuild is not visible, as portage words it; empty when it is."""
    from portage.package.ebuild.config import config
    from portage.package.ebuild.getmaskingstatus import getmaskingstatus

    settings = config(clone=portdb.settings)
    return tuple(getmaskingstatus(cpv, settings=settings, portdb=portdb, myrepo=repo))


def best_visible(portdb, cp):
    """{slot: best visible cpv} over every repository's versions of cp."""
    from portage import best

    by_slot = {}
    for cpv in portdb.xmatch("match-visible", cp):
        (slot,) = portdb.aux_get(cpv, ["SLOT"])
        by_slot.setdefault(slot.partition("/")[0], []).append(cpv)
    return {slot: str(best(cpvs)) for slot, cpvs in sorted(by_slot.items())}
