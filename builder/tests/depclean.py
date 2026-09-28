"""Portage's own depclean, as the oracle for `egraph orphans` and `egraph why`.

This is `emerge --depclean --pretend` with the output left out. It is test code, so unlike the
builder it may reach into _emerge: the answer has to be emerge's.
"""

from typing import NamedTuple

import portage


class Depclean(NamedTuple):
    # Nonzero when depclean refuses to run: no @world, or unresolved runtime dependencies.
    returncode: int
    # Installed cpvs depclean keeps.
    kept: frozenset
    # Installed cpvs it would remove, sorted.
    orphans: tuple
    # (parent cpv, atom) for each runtime dependency it could not resolve; any makes it refuse.
    unresolved: frozenset
    # child cpv -> {(parent, atom)}: what pulled each kept package in. A parent is a cpv, or
    # "@name" for a root set.
    parents: dict


def depclean(trees, eroot, with_bdeps=True, dynamic_deps=False):
    """depclean's answer, with --dynamic-deps=n unless dynamic_deps."""
    import _emerge.emergelog
    from _emerge.actions import _calc_depclean
    from _emerge.Package import Package
    from _emerge.SetArg import SetArg
    from _emerge.UnmergeDepPriority import UnmergeDepPriority
    from portage._sets.base import InternalPackageSet

    options = {"--pretend": True}
    if not with_bdeps:
        options["--with-bdeps"] = "n"
    if not dynamic_deps:
        options["--dynamic-deps"] = "n"
    settings = trees[eroot]["root_config"].settings
    noiselimit = portage.util.noiselimit
    disabled = _emerge.emergelog._disable
    portage.util.noiselimit = -2
    _emerge.emergelog._disable = True
    try:
        result = _calc_depclean(
            settings, trees, None, options, "depclean", InternalPackageSet(), None
        )
    finally:
        portage.util.noiselimit = noiselimit
        _emerge.emergelog._disable = disabled

    if result.depgraph is None:
        # The sets could not be loaded, or @world is empty.
        return Depclean(result.returncode, frozenset(), (), frozenset(), {})
    dynamic = result.depgraph._dynamic_config
    kept = frozenset(node.cpv for node in dynamic.digraph if isinstance(node, Package))
    installed = sorted(str(cpv) for cpv in trees[eroot]["vartree"].dbapi.cpv_all())
    unresolved = frozenset(
        (dep.parent.cpv, str(getattr(dep.atom, "unevaluated_atom", dep.atom)))
        for dep in dynamic._initially_unsatisfied_deps
        if isinstance(dep.parent, Package)
        and dep.priority > UnmergeDepPriority.SOFT
        and not dep.atom.soname
    )
    parents = {}
    for child, pairs in dynamic._parent_atoms.items():
        if not isinstance(child, Package):
            continue
        for parent, atom in pairs:
            if isinstance(parent, SetArg):
                name = "@" + parent.name
            elif isinstance(parent, Package):
                name = parent.cpv
            else:
                continue
            parents.setdefault(child.cpv, set()).add((name, str(atom)))
    return Depclean(
        result.returncode,
        kept,
        tuple(cpv for cpv in installed if cpv not in kept),
        unresolved,
        parents,
    )
