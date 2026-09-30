"""Portage's own `emerge --pretend --update @installed`, as the oracle for `egraph updates`.

@installed holds a slot atom per installed package, so this asks emerge what it would replace
each installed package with within its slot. Test code, so it may reach into _emerge.
"""

import re
import types
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
    # Rebuilds of an installed version from its own repository that no flag triggered: slot
    # operators'.
    rebuilt: frozenset
    # Packages merged in a slot where nothing is installed.
    new: frozenset
    # The cpvs merged, in emerge's merge order.
    order: tuple = ()
    # (installed cpv it replaces or "", cpv, repo) for every merge.
    merges: frozenset = frozenset()
    # New cpv -> the USE emerge --verbose shows for it.
    use: dict = None
    # The installed cpvs uninstalled for blockers, when it succeeds.
    uninstalls: frozenset = frozenset()
    # (atom without its "!"s, holder cpv) for each blocker it cannot resolve.
    blocks: frozenset = frozenset()
    # It fails for those blockers alone.
    blocked: bool = False
    # The atoms it shows no visible version satisfies, when it fails for them alone.
    unsatisfied: frozenset = frozenset()
    # "cpv::repo" of the versions it shows REQUIRED_USE unmet for.
    unmet: frozenset = frozenset()


def updates(
    trees,
    eroot,
    newuse=False,
    changed_use=False,
    deep=False,
    target="@installed",
    dynamic_deps=True,
    update=True,
    noreplace=False,
):
    """What emerge -pu target (with -N or -U, and -D) replaces, rebuilds and adds. target may
    be several arguments. Without update, plain emerge's, or emerge -n's with noreplace.
    """
    import _emerge.emergelog
    from _emerge.actions import expand_set_arguments
    from _emerge.create_depgraph_params import create_depgraph_params
    from _emerge.depgraph import _frozen_depgraph_config, backtrack_depgraph
    from _emerge.Package import Package
    from _emerge.SetArg import SetArg

    options = {
        "--pretend": True,
        "--dynamic-deps": "y" if dynamic_deps else "n",
    }
    if update:
        options["--update"] = True
    if noreplace:
        options["--noreplace"] = True
    if deep:
        options["--deep"] = True
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
        arguments = [target] if isinstance(target, str) else list(target)
        atoms, _ = expand_set_arguments(arguments, None, root_config)
        success, depgraph, _ = backtrack_depgraph(
            settings, trees, options, params, None, atoms, None, frozen_config=frozen
        )
    finally:
        portage.util.noiselimit = noiselimit
        _emerge.emergelog._disable = disabled

    reinstall = depgraph._dynamic_config._reinstall_nodes
    replaced, rebuilt, new, merges, use = {}, set(), set(), set(), {}
    for pkg in depgraph._dynamic_config.digraph:
        if not isinstance(pkg, Package) or pkg.installed or pkg.onlydeps:
            continue
        if pkg.root != eroot or pkg.operation != "merge":
            continue
        installed = vardb.match(pkg.slot_atom)
        merges.add((str(installed[0]) if installed else "", str(pkg.cpv), pkg.repo))
        if not installed:
            new.add(pkg.cpv)
            use[str(pkg.cpv)] = use_string(depgraph, pkg)
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
    tasks = depgraph.altlist() if success else ()
    order = tuple(
        pkg.cpv
        for pkg in tasks
        if isinstance(pkg, Package)
        and not pkg.installed
        and pkg.root == eroot
        and pkg.operation == "merge"
    )
    uninstalls = frozenset(
        str(pkg.cpv)
        for pkg in tasks
        if isinstance(pkg, Package) and pkg.operation == "uninstall"
    )
    dynamic = depgraph._dynamic_config
    unsolved = dynamic._unsatisfied_blockers_for_display or ()
    blocks = frozenset(
        (str(blocker.atom).lstrip("!"), str(parent.cpv))
        for blocker in unsolved
        for parent in dynamic._blocker_parents.parent_nodes(blocker)
    )
    conflicts = any(dynamic._package_tracker.slot_conflicts())
    blocked = not success and bool(blocks) and not conflicts
    # An installed package plain emerge has no ebuild of, named by a set: a warning for the
    # root sets, and for the sets nested in @selected, which egraph counts as @selected (a quirk
    # upstream-notes.md records).
    lenient = {"selected", "system", "world"} | {
        name.lstrip("@") for name in root_config.sets["selected"].getNonAtoms()
    }
    missing = [
        atom
        for (_, atom), details in dynamic._unsatisfied_deps_for_display
        if not details.get("show_req_use")
        and not (
            isinstance(details.get("myparent"), SetArg)
            and details["myparent"].name in lenient
            and vardb.match(atom)
        )
    ]
    unsatisfied = frozenset()
    if (
        not success
        and missing
        and not blocks
        and not conflicts
        and not dynamic._required_use_unsatisfied
        and not dynamic._needed_use_config_changes
    ):
        unsatisfied = frozenset(str(atom) for atom in missing)
    unmet = frozenset(
        f"{details['show_req_use'].cpv}::{details['show_req_use'].repo}"
        for _, details in dynamic._unsatisfied_deps_for_display
        if details.get("show_req_use")
    )
    return Updates(
        success,
        replaced,
        frozenset(rebuilt),
        frozenset(new),
        order,
        frozenset(merges),
        use,
        uninstalls,
        blocks,
        blocked,
        unsatisfied,
        unmet,
    )


def use_string(depgraph, pkg):
    """The USE emerge --verbose shows for pkg as a new package, uncoloured."""
    from _emerge.resolver.output import Display

    display = Display.__new__(Display)
    display.conf = types.SimpleNamespace(
        print_use_string=True,
        alphabetical=False,
        all_flags=False,
        pkg_use_enabled=depgraph._pkg_use_enabled,
        reinstall_nodes={},
    )
    display.verboseadd = ""
    display._display_use(pkg, types.SimpleNamespace(previous_pkg=None))
    return re.sub(r"\x1b\[[0-9;]*m", "", display.verboseadd).strip()


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


def candidate_matches(trees, eroot, atom, candidates):
    """The candidates ("cpv::repo") that atom matches as depgraph matches an ebuild against a
    dependency: its Package, with the USE it would be built with now."""
    from _emerge.Package import Package
    from portage.dep import Atom, match_from_list

    root_config = trees[eroot]["root_config"]
    portdb = trees[eroot]["porttree"].dbapi
    keys = list(portdb._aux_cache_keys)
    wanted = Atom(atom, allow_repo=True)
    found = set()
    for candidate in candidates:
        if candidate.cp != wanted.cp:
            continue
        metadata = zip(keys, portdb.aux_get(candidate.cpv, keys, myrepo=candidate.repo))
        pkg = Package(
            built=False,
            cpv=candidate.cpv,
            installed=False,
            metadata=metadata,
            root_config=root_config,
            type_name="ebuild",
        )
        if match_from_list(wanted, [pkg]):
            found.add(f"{candidate.cpv}::{candidate.repo}")
    return found


def invalid_reasons(trees, eroot, cpv, repo):
    """The "invalid: ..." mask reasons depgraph gives the ebuild of cpv in repo."""
    from _emerge.Package import Package

    root_config = trees[eroot]["root_config"]
    portdb = trees[eroot]["porttree"].dbapi
    keys = list(portdb._aux_cache_keys)
    pkg = Package(
        built=False,
        cpv=cpv,
        installed=False,
        metadata=zip(keys, portdb.aux_get(cpv, keys, myrepo=repo)),
        root_config=root_config,
        type_name="ebuild",
    )
    return tuple(
        f"invalid: {message}"
        for messages in (pkg.invalid or {}).values()
        for message in messages
    )


def candidate_deps(trees, eroot, candidate):
    """{kind: atoms} of a candidate ebuild as depgraph reduces them: its Package's dependency
    strings under the USE it would be built with now."""
    from _emerge.Package import Package
    from portage.dep import Atom, use_reduce

    root_config = trees[eroot]["root_config"]
    portdb = trees[eroot]["porttree"].dbapi
    keys = list(portdb._aux_cache_keys)
    metadata = zip(keys, portdb.aux_get(candidate.cpv, keys, myrepo=candidate.repo))
    pkg = Package(
        built=False,
        cpv=candidate.cpv,
        installed=False,
        metadata=metadata,
        root_config=root_config,
        type_name="ebuild",
    )
    found = {}
    for kind in Package._dep_keys:
        tokens = use_reduce(
            pkg._metadata[kind],
            uselist=pkg.use.enabled,
            eapi=pkg.eapi,
            flat=True,
            token_class=Atom,
        )
        found[kind] = frozenset(str(t) for t in tokens if t != "||")
    return found
