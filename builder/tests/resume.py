"""Portage's own `emerge --resume --pretend`, as the oracle for the resume lists egraph writes.

Test code, so it may reach into _emerge.
"""

from typing import NamedTuple

import portage


class Resumed(NamedTuple):
    success: bool
    # (cpv, repo) for every merge.
    merges: frozenset
    # The installed cpvs uninstalled for blockers.
    uninstalls: frozenset
    # The merges emerge dropped from the list for dependencies it left unsatisfied, by cpv.
    dropped: frozenset
    # The cpvs merged, in emerge's merge order.
    order: tuple


def resumed(trees, eroot, entry):
    """What emerge --resume --pretend merges and uninstalls for entry, mtimedb's resume entry as
    egraph writes it, with the options emerge's command line would add to it."""
    import _emerge.emergelog
    from _emerge.create_depgraph_params import create_depgraph_params
    from _emerge.depgraph import _resume_depgraph
    from _emerge.Package import Package

    options = {**entry["myopts"], "--pretend": True, "--resume": True}
    mtimedb = {"resume": {key: value for key, value in entry.items()}}
    noiselimit = portage.util.noiselimit
    disabled = _emerge.emergelog._disable
    portage.util.noiselimit = -2
    _emerge.emergelog._disable = True
    try:
        params = create_depgraph_params(options, "resume")
        success, depgraph, dropped = _resume_depgraph(
            trees[eroot]["root_config"].settings,
            trees,
            mtimedb,
            options,
            params,
            None,
        )
        tasks = depgraph.altlist() if success else ()
    finally:
        portage.util.noiselimit = noiselimit
        _emerge.emergelog._disable = disabled
    merged = [
        pkg
        for pkg in tasks
        if isinstance(pkg, Package) and pkg.operation == "merge" and not pkg.installed
    ]
    return Resumed(
        success=success,
        merges=frozenset((str(pkg.cpv), pkg.repo) for pkg in merged),
        uninstalls=frozenset(
            str(pkg.cpv)
            for pkg in tasks
            if isinstance(pkg, Package) and pkg.operation == "uninstall"
        ),
        dropped=frozenset(str(task.cpv) for task in dropped),
        order=tuple(str(pkg.cpv) for pkg in merged),
    )
