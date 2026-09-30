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


def _dependency_strings(vardb, cpv, portdb):
    """({kind: dependency string}, EAPI): the vdb's, or with portdb as --dynamic-deps=y reads them."""
    if portdb is None:
        keys = list(DEP_KINDS) + ["EAPI"]
        found = dict(zip(keys, vardb.aux_get(cpv, keys)))
        return found, found["EAPI"]
    _, strings, eapi = dynamic.dependency_strings(vardb, portdb, cpv)
    return strings, eapi


def _reduced(vardb, cpv, kind, portdb=None):
    strings, eapi = _dependency_strings(vardb, cpv, portdb)
    (use,) = vardb.aux_get(cpv, ["USE"])
    return use_reduce(
        strings[kind],
        uselist=frozenset(use.split()),
        eapi=eapi or None,
        opconvert=False,
        token_class=Atom,
    )


def dep_atoms(vardb, cpv, kind, portdb=None):
    """(Atom, choice) for every atom left in one dependency kind after USE reduction.

    With portdb, the dependencies are the ones emerge's default --dynamic-deps=y reads.
    Raises InvalidDependString or InvalidAtom when portage cannot parse it.
    """
    out = []
    _flatten(_reduced(vardb, cpv, kind, portdb), False, out)
    return out


def errors(vardb, portdb=None):
    """(cpv, key) of every dependency or soname string portage cannot parse."""
    found = set()
    for cpv in installed(vardb):
        for kind in DEP_KINDS:
            try:
                dep_atoms(vardb, cpv, kind, portdb)
            except (InvalidAtom, InvalidDependString):
                found.add((cpv, kind))
        for key in ("PROVIDES", "REQUIRES"):
            (value,) = vardb.aux_get(cpv, [key])
            try:
                tuple(parse_soname_deps(value))
            except InvalidData:
                found.add((cpv, key))
    return frozenset(found)


def deps(vardb, cpv, kinds=DEP_KINDS, portdb=None):
    """Edges from cpv to the installed packages its atoms match.

    Blockers are constraints rather than dependencies and yield no edges. A
    kind portage cannot parse yields none either; errors() reports it.
    """
    edges = set()
    for kind in kinds:
        try:
            atoms = dep_atoms(vardb, cpv, kind, portdb)
        except (InvalidAtom, InvalidDependString):
            continue
        for atom, choice in atoms:
            if atom.blocker:
                continue
            for child in matches(vardb, atom):
                edges.add(Edge(cpv, child, kind, str(atom), choice))
    return frozenset(edges)


def rdeps(vardb, cpv, kinds=DEP_KINDS, portdb=None):
    """Edges into cpv, found by evaluating every installed package."""
    return frozenset(
        edge
        for parent in installed(vardb)
        for edge in deps(vardb, parent, kinds, portdb)
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


def broken(vardb, portdb=None):
    """(cpv, kind, dependency) for each top-level dependency nothing installed satisfies.

    Evaluated straight from portage's reduced dependency lists, with the dependency rendered
    by paren_enclose. Blockers are constraints, not dependencies, and never count.
    """
    found = set()
    for cpv in installed(vardb):
        for kind in DEP_KINDS:
            try:
                tokens = iter(_reduced(vardb, cpv, kind, portdb))
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


def blockers(vardb, portdb=None):
    """(holder, kind, blocker, blocked) for each blocker left in an installed package's reduced
    dependencies, at any depth, and each installed package vardb matches its atom to; blocked
    is "" when it matches nothing."""
    found = set()
    for cpv in installed(vardb):
        for kind in DEP_KINDS:
            try:
                atoms = dep_atoms(vardb, cpv, kind, portdb)
            except (InvalidAtom, InvalidDependString):
                continue
            for atom, _ in atoms:
                if not atom.blocker:
                    continue
                blocked = vardb.match(Atom(str(atom).lstrip("!")))
                for other in blocked or [""]:
                    found.add((cpv, kind, str(atom), str(other)))
    return frozenset(found)


# The repository side: what emerge sees of an installed package through its ebuild, and of the
# versions its repositories offer. Each takes the ebuild repositories' portdbapi beside the vardb.


def dynamic_dep_strings(vardb, portdb, cpv):
    """(source, {kind: dependency string}, EAPI) as emerge's default --dynamic-deps=y sees cpv."""
    return dynamic.dependency_strings(vardb, portdb, cpv)


def dynamic_dep_atoms(vardb, portdb, cpv, kind):
    return dep_atoms(vardb, cpv, kind, portdb)


def dynamic_deps(vardb, portdb, cpv, kinds=DEP_KINDS):
    return deps(vardb, cpv, kinds, portdb)


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


def best_visible(portdb, cp, invalid=lambda cpv: False):
    """{slot: best visible cpv} over every repository's versions of cp, less those invalid
    holds for (a cpv with its repo), which match-visible counts but depgraph masks."""
    from portage import best

    by_slot = {}
    for cpv in portdb.xmatch("match-visible", cp):
        if invalid(cpv):
            continue
        (slot,) = portdb.aux_get(cpv, ["SLOT"])
        by_slot.setdefault(slot.partition("/")[0], []).append(cpv)
    return {slot: str(best(cpvs)) for slot, cpvs in sorted(by_slot.items())}


# Possible dependencies: what an installed package's ebuild would add with flags toggled that its
# installed build left as they are. "flag" turns a flag on, "-flag" turns it off.


def use_toggles(vardb, portdb, cpv):
    """The toggles the user could make on cpv's ebuild: its explicit IUSE, less what the profile
    masks (for a flag that is off) or forces (for a flag that is on)."""
    from portage.package.ebuild.config import config

    use, repo = vardb.aux_get(cpv, ["USE", "repository"])
    keys = ["EAPI", "IUSE", "KEYWORDS", "SLOT", "repository"]
    metadata = dict(zip(keys, portdb.aux_get(cpv, keys, myrepo=repo or None)))
    settings = config(clone=portdb.settings)
    settings.setcpv(cpv, mydb=metadata)
    enabled = frozenset(use.split())
    toggles = []
    for flag in sorted({flag.lstrip("+-") for flag in metadata["IUSE"].split()}):
        if flag in enabled:
            if flag not in settings.useforce:
                toggles.append(f"-{flag}")
        elif flag not in settings.usemask:
            toggles.append(flag)
    return tuple(toggles)


def possible_atoms(vardb, portdb, cpv, kind, toggles):
    """(Atom, choice) of one kind that toggles add: the atoms use_reduce selects as conditional
    on the toggled flags (its subset), under the installed USE with toggles applied, less the
    atoms the package already depends on. Blockers left out, and none at all unless emerge reads
    the ebuild's dependencies (only those keep their conditionals).

    Raises InvalidDependString or InvalidAtom when portage cannot parse the string.
    """
    source, strings, eapi = dynamic.dependency_strings(vardb, portdb, cpv)
    if source != "ebuild" or not toggles:
        return []
    (use,) = vardb.aux_get(cpv, ["USE"])
    use = set(use.split())
    subset = set()
    for toggle in toggles:
        if toggle.startswith("-"):
            use.discard(toggle[1:])
            subset.add(f"!{toggle[1:]}")
        else:
            use.add(toggle)
            subset.add(toggle)
    tokens = use_reduce(
        strings[kind],
        uselist=frozenset(use),
        eapi=eapi or None,
        opconvert=False,
        token_class=Atom,
        subset=subset,
    )
    selected = []
    _flatten(tokens, False, selected)
    present = {str(atom) for atom, _ in dep_atoms(vardb, cpv, kind, portdb)}
    return [
        (atom, choice)
        for atom, choice in selected
        if not atom.blocker and str(atom) not in present
    ]


def possible(vardb, portdb, cpv, toggles, kinds=DEP_KINDS):
    """Edges from cpv to the installed packages the atoms toggles add would match."""
    edges = set()
    for kind in kinds:
        try:
            atoms = possible_atoms(vardb, portdb, cpv, kind, toggles)
        except (InvalidAtom, InvalidDependString):
            continue
        for atom, choice in atoms:
            for child in matches(vardb, atom):
                edges.add(Edge(cpv, child, kind, str(atom), choice))
    return frozenset(edges)
