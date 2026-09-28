"""Portage's own `emerge --pretend --update @installed`, as the oracle for `egraph updates`.

@installed holds a slot atom per installed package, so this asks emerge what it would replace
each installed package with within its slot. Test code, so it may reach into _emerge.
"""

from typing import NamedTuple

import portage


class Replacement(NamedTuple):
    cpv: str
    repo: str
    # The flags --newuse or --changed-use rebuild the installed version for; None when the
    # replacement is not such a rebuild.
    flags: frozenset = None


class Updates(NamedTuple):
    success: bool
    # Installed cpv -> Replacement.
    replaced: dict
    # Rebuilds of an installed version from its own repository that no flag triggered (slot
    # operators, for one): a resolver matter egraph leaves out.
    rebuilt: frozenset
    # Packages merged in a slot where nothing is installed.
    new: frozenset


def updates(trees, eroot, newuse=False, changed_use=False):
    """What emerge -pu @installed (with -N or -U) replaces, rebuilds and adds."""
    import _emerge.emergelog
    from _emerge.actions import expand_set_arguments
    from _emerge.create_depgraph_params import create_depgraph_params
    from _emerge.depgraph import _frozen_depgraph_config, backtrack_depgraph
    from _emerge.Package import Package

    options = {"--pretend": True, "--update": True}
    if newuse:
        options["--newuse"] = True
    if changed_use:
        options["--reinstall"] = "changed-use"
    root_config = trees[eroot]["root_config"]
    settings = root_config.settings
    vardb = trees[eroot]["vartree"].dbapi
    noiselimit = portage.util.noiselimit
    disabled = _emerge.emergelog._disable
    portage.util.noiselimit = -2
    _emerge.emergelog._disable = True
    try:
        params = create_depgraph_params(options, None)
        frozen = _frozen_depgraph_config(settings, trees, options, params, None)
        atoms, _ = expand_set_arguments(["@installed"], None, root_config)
        success, depgraph, _ = backtrack_depgraph(
            settings, trees, options, params, None, atoms, None, frozen_config=frozen
        )
    finally:
        portage.util.noiselimit = noiselimit
        _emerge.emergelog._disable = disabled

    reinstall = depgraph._dynamic_config._reinstall_nodes
    replaced, rebuilt, new = {}, set(), set()
    for pkg in depgraph._dynamic_config.digraph:
        if not isinstance(pkg, Package) or pkg.installed or pkg.onlydeps:
            continue
        if pkg.root != eroot or pkg.operation != "merge":
            continue
        installed = vardb.match(pkg.slot_atom)
        if not installed:
            new.add(pkg.cpv)
            continue
        (old,) = installed
        flags = reinstall.get(pkg)
        (repo,) = vardb.aux_get(old, ["repository"])
        if old == pkg.cpv and repo == pkg.repo and flags is None:
            rebuilt.add(str(old))
            continue
        replaced[str(old)] = Replacement(
            pkg.cpv, pkg.repo, None if flags is None else frozenset(flags)
        )
    return Updates(success, replaced, frozenset(rebuilt), frozenset(new))


def equiv_visible(trees, eroot):
    """{installed cpv: whether depgraph finds a visible ebuild of its version}."""
    from _emerge.create_depgraph_params import create_depgraph_params
    from _emerge.depgraph import depgraph

    root_config = trees[eroot]["root_config"]
    options = {"--pretend": True}
    graph = depgraph(
        root_config.settings,
        trees,
        options,
        create_depgraph_params(options, None),
        None,
    )
    graph._load_vdb()
    vardb = trees[eroot]["vartree"].dbapi
    return {
        str(cpv): graph._equiv_ebuild_visible(
            graph._pkg(cpv, "installed", root_config, installed=True)
        )
        for cpv in vardb.cpv_all()
    }


def masked(trees, eroot, dynamic_deps=True):
    """{installed cpv: whether depgraph finds it masked (Package.masks)}, reading installed
    packages with --dynamic-deps as given."""
    from _emerge.create_depgraph_params import create_depgraph_params
    from _emerge.depgraph import depgraph

    root_config = trees[eroot]["root_config"]
    options = {"--pretend": True, "--dynamic-deps": "y" if dynamic_deps else "n"}
    graph = depgraph(
        root_config.settings,
        trees,
        options,
        create_depgraph_params(options, None),
        None,
    )
    graph._load_vdb()
    vardb = trees[eroot]["vartree"].dbapi
    return {
        str(cpv): bool(graph._pkg(cpv, "installed", root_config, installed=True).masks)
        for cpv in vardb.cpv_all()
    }
