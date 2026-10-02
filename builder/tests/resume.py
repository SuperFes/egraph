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


class Scheduled(NamedTuple):
    success: bool
    # The cpvs merged, in emerge's merge order.
    order: tuple
    # (cpv, cpv it depends on) -> the kinds of the scheduler graph's edge between the two merges,
    # in egraph's letters (plan.hpp's wait_letters); "?" for a priority with no letter.
    edges: dict
    # (cpv, cpv) where the second merge is reachable from the first through installed packages
    # emerge leaves alone, and nothing else.
    through: frozenset
    # cpv -> the USE emerge merges it with.
    use: dict
    # Installed cpv uninstalled -> the cpvs of the merges it waits for. A replaced version's
    # uninstall, which emerge drops for the merge, is left out.
    uninstalls: dict


def resume_depgraph(trees, eroot, entry):
    """(success, depgraph, dropped tasks, merge list) of emerge --resume --pretend for entry,
    mtimedb's resume entry as egraph writes it, with the options emerge's command line would add
    to it."""
    import _emerge.emergelog
    from _emerge.create_depgraph_params import create_depgraph_params
    from _emerge.depgraph import _resume_depgraph

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
    return success, depgraph, dropped, tasks


def merges_in(tasks):
    from _emerge.Package import Package

    return [
        pkg
        for pkg in tasks
        if isinstance(pkg, Package) and pkg.operation == "merge" and not pkg.installed
    ]


def letters(priorities):
    """A scheduler graph edge's priorities as egraph's wait letters."""
    from _emerge.DepPriority import DepPriority

    found = set()
    for priority in priorities:
        if not isinstance(priority, DepPriority) or priority.optional:
            found.add("?")
        elif priority.installtime:
            found.add("i")
        elif priority.buildtime or priority.buildtime_slot_op:
            found.add("b")
        elif priority.runtime or priority.runtime_slot_op:
            found.add("r")
        elif priority.runtime_post:
            found.add("p")
        else:
            found.add("?")
    return found


def scheduled(trees, eroot, entry):
    """The scheduler graph emerge --resume would run entry by, between its merges: as
    schedulerGraph() makes it, with the implicit libc waits it adds."""
    from _emerge.Package import Package

    success, depgraph, _, tasks = resume_depgraph(trees, eroot, entry)
    if not success:
        return Scheduled(False, (), {}, frozenset(), {}, {})
    graph = depgraph._dynamic_config._scheduler_graph
    merged = set(merges_in(tasks))
    explicit = {
        (pkg, child): list(graph.nodes[pkg][0][child])
        for pkg in merged
        for child in graph.child_nodes(pkg)
    }
    depgraph._implicit_libc_deps(tasks, graph)
    edges, through = {}, set()
    for pkg in merged:
        for child in graph.child_nodes(pkg):
            if child not in merged:
                continue
            priorities = graph.nodes[pkg][0][child]
            before = explicit.get((pkg, child), [])
            found = letters(before)
            if any(all(p is not q for q in before) for p in priorities):
                found.add("l")
            edges[(str(pkg.cpv), str(child.cpv))] = "".join(
                letter for letter in "birpl?" if letter in found
            )
        seen = set()
        stack = [
            child
            for child in graph.child_nodes(pkg)
            if child.installed and child.operation == "nomerge"
        ]
        while stack:
            node = stack.pop()
            if node in seen:
                continue
            seen.add(node)
            for child in graph.child_nodes(node):
                if child in merged:
                    if child is not pkg:
                        through.add((str(pkg.cpv), str(child.cpv)))
                elif child.installed and child.operation == "nomerge":
                    stack.append(child)
    return Scheduled(
        True,
        tuple(str(pkg.cpv) for pkg in merges_in(tasks)),
        edges,
        frozenset(through),
        {str(pkg.cpv): frozenset(pkg.use.enabled) for pkg in merged},
        {
            str(pkg.cpv): frozenset(
                str(child.cpv) for child in graph.child_nodes(pkg) if child in merged
            )
            for pkg in tasks
            if isinstance(pkg, Package) and pkg.operation == "uninstall"
        },
    )


def resumed(trees, eroot, entry):
    """What emerge --resume --pretend merges and uninstalls for entry (resume_depgraph)."""
    from _emerge.Package import Package

    success, _, dropped, tasks = resume_depgraph(trees, eroot, entry)
    merged = merges_in(tasks)
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


def worker_requests(trees, eroot, entry, requests):
    """What emerge --resume, running entry as egraph's requests order it, would hand each step:
    a merge's blockers (as its scheduler finds them, the installed packages counted as its
    BlockerDB counts them once each step before is done) and world atom (create_world_atom's,
    before the run), and whether an uninstall cleans the world file; as the requests spell them.
    None when emerge refuses entry."""
    from _emerge.BlockerDB import BlockerDB
    from _emerge.create_world_atom import create_world_atom
    from portage._sets.base import InternalPackageSet
    from portage.dep import Atom

    success, depgraph, _, tasks = resume_depgraph(trees, eroot, entry)
    if not success:
        return None
    # emerge picks among equal versions in directory order, egraph by its own.
    merges = {}
    spelled = {r["cpv"] for r in requests if "cpv" in r}
    for pkg in merges_in(tasks):
        merges[same_version(spelled)(str(pkg.cpv))] = pkg
    vartree = depgraph.schedulerGraph().trees[eroot]["vartree"]
    blocker_db = BlockerDB(vartree)
    root_config = trees[eroot]["root_config"]
    args_set = InternalPackageSet(entry["favorites"], allow_repo=True)
    oneshot = "--oneshot" in entry["myopts"]
    expected = []
    for request in requests:
        if "uninstall" in request:
            (pkg,) = vartree.dbapi.match_pkgs(Atom("=" + request["uninstall"]))
            step = {"uninstall": request["uninstall"]}
            if not oneshot and args_set.findAtomForPackage(pkg):
                step["clean_world"] = True
        else:
            pkg = merges[request["cpv"]]
            if str(pkg.cpv) != request["cpv"]:
                # Resuming, emerge took another ebuild of an equal version, perhaps in another
                # slot: nothing to hold the request to.
                expected.append(request)
                blocker_db.discardBlocker(pkg)
                continue
            step = {"cpv": request["cpv"], "repo": pkg.repo}
            blockers = sorted(
                str(blocker.cpv)
                for blocker in blocker_db.findInstalledBlockers(pkg)
                if blocker.slot_atom != pkg.slot_atom and blocker.cpv != pkg.cpv
            )
            if blockers:
                step["blockers"] = blockers
            atom = None
            if not oneshot:
                atom = create_world_atom(
                    pkg, args_set, root_config, before_install=True
                )
            if atom is not None:
                step["world"] = str(atom)
        expected.append(step)
        blocker_db.discardBlocker(pkg)
    return expected


def same_version(cpvs):
    """A cpv as spelled among cpvs: emerge picks among equal versions in directory order."""
    from portage.versions import cpv_getkey, vercmp

    def spell(cpv):
        if cpv in cpvs:
            return cpv
        cp = cpv_getkey(cpv)
        return next(
            (
                other
                for other in cpvs
                if cpv_getkey(other) == cp
                and vercmp(other[len(cp) + 1 :], cpv[len(cp) + 1 :]) == 0
            ),
            cpv,
        )

    return spell
