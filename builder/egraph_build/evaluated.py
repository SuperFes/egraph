"""The evaluated layer: the installed packages as emerge sees them against the repositories.

Per installed package, its dependency trees under emerge's default --dynamic-deps=y, matched
against the installed packages, and what its ebuild would add with flags toggled; per installed
cp, the versions it could move to, with the USE each would be built with now. Portage does all
the evaluation, as for the installed layer.
"""

import collections
import itertools
import json
from typing import NamedTuple

from portage.dep import Atom, use_reduce
from portage.eapi import _get_eapi_attrs
from portage.exception import InvalidAtom, InvalidDependString
from portage.versions import cpv_getkey

from egraph_build import dynamic, installed, masks
from egraph_build.model import DEP_KINDS

SOURCES = dynamic.SOURCES
EBUILD, VDB, MOVED = range(len(SOURCES))

# What config.setcpv reads to compute a package's USE; all of it is ebuild metadata.
_CANDIDATE_KEYS = (
    "BDEPEND",
    "DEFINED_PHASES",
    "DEPEND",
    "EAPI",
    "IDEPEND",
    "INHERITED",
    "IUSE",
    "KEYWORDS",
    "LICENSE",
    "PDEPEND",
    "PROPERTIES",
    "RDEPEND",
    "REQUIRED_USE",
    "RESTRICT",
    "SLOT",
    "SRC_URI",
    "repository",
)


class Possible(NamedTuple):
    """An atom the ebuild's dependencies would add with flags toggled."""

    kind: str
    atom: str
    # Inside a || group.
    choice: bool
    # Installed cpvs it matches.
    matches: tuple
    # The fewest toggles that add it: "flag" turns a flag on, "-flag" turns one off.
    flags: tuple


class Dependencies(NamedTuple):
    cpv: str
    # EBUILD, VDB or MOVED: where the strings came from (dynamic.dependency_strings).
    source: int
    # The EAPI the strings were read with.
    eapi: str
    # (kind, message) for every dependency string portage could not parse.
    errors: tuple
    # One node tuple per kind, as in installed.Package, reduced under the installed USE.
    deps: tuple
    # Possible entries sorted by kind, atom, choice and flags; only the ebuild's strings keep
    # the conditionals they come from.
    possible: tuple = ()
    # An ebuild of the same version is visible, as emerge requires of an installed package it
    # keeps when an ebuild in its slot is visible (depgraph's _equiv_ebuild_visible).
    visible: bool = True
    # Its installed metadata is masked (keywords, package.mask, license and the like), as
    # depgraph's Package.masks has it under --dynamic-deps=y, which reads the EAPI, KEYWORDS and
    # dependencies of the same version's ebuild when source is EBUILD: depclean passes over it
    # unless visible. Only for a package depclean weighs it for, one not visible or beside
    # another installed version of its cp; False for the rest.
    masked: bool = False
    # The same under --dynamic-deps=n, from the vdb alone.
    vdb_masked: bool = False
    # (cpv, repo) of the best visible version in its slot, when emerge -u would replace it with
    # that (a different version, or any when it is not visible) or --newuse would rebuild it.
    target: tuple = None
    # The flags --newuse rebuilds it for when target is not a replacement, as emerge shows
    # them: "flag*" or "-flag*" changed, "flag%*" or "-flag%" new in IUSE, "(-flag%*)" or
    # "(-flag%)" gone from it (* when it was on). Those with a * are --changed-use's.
    rebuild: tuple = ()


# Candidate.deps of a masked candidate: one empty node tuple per kind.
NO_DEPS = ((),) * len(DEP_KINDS)


class Candidate(NamedTuple):
    """One version of a cp in one repository: an installed cp, or one the dependencies reach
    that nothing installed satisfies."""

    cp: str
    cpv: str
    repo: str
    slot: str
    sub_slot: str
    # The flags the ebuild would be built with now, within its IUSE.
    use: tuple
    # Without + and - defaults.
    iuse: tuple
    # Of its IUSE, the flags the profile masks or forces.
    forced: tuple
    # Why it is masked, as portage words it; empty when it is visible.
    reasons: tuple
    # (kind, message) for every dependency string portage could not parse.
    errors: tuple = ()
    # One node tuple per kind, as in installed.Package, reduced under use; matches name
    # installed cpvs. Empty lists for a masked candidate.
    deps: tuple = NO_DEPS
    # REQUIRED_USE's tokens as check_required_use splits them; empty for a masked candidate or
    # where its EAPI has none.
    required_use: tuple = ()
    # Its EAPI's empty_groups_always_true; False for a masked candidate.
    empty_groups_true: bool = False
    # Per kind, its dependency string's tokens as use_reduce splits them, for USE it could be
    # built with instead; empty for a masked candidate.
    tokens: tuple = NO_DEPS


def _cpv(pkg):
    return pkg.cpv


def _candidate_key(candidate):
    return (candidate.cp, candidate.cpv, candidate.repo)


class EvaluatedLayer:
    def __init__(
        self,
        packages,
        candidates,
        repository_cps=(),
        requested=(),
        use_expand=(),
        use_expand_hidden=(),
    ):
        self._packages = {pkg.cpv: pkg for pkg in sorted(packages, key=_cpv)}
        self._candidates = tuple(sorted(candidates, key=_candidate_key))
        self._repository_cps = tuple(sorted(repository_cps))
        self._requested = tuple(sorted(requested))
        self._use_expand = tuple(sorted(set(use_expand)))
        self._use_expand_hidden = tuple(sorted(set(use_expand_hidden)))

    def __iter__(self):
        return iter(self._packages.values())

    def package(self, cpv):
        return self._packages[cpv]

    def installed(self):
        return tuple(self._packages)

    def cps(self):
        """The cps it answers for, sorted: the installed ones and those with candidates."""
        return sorted(
            {cpv_getkey(cpv) for cpv in self._packages}
            | {c.cp for c in self._candidates}
        )

    def repository_cps(self):
        """Every cp with an ebuild in a repository, sorted."""
        return self._repository_cps

    def requested(self):
        """The cps evaluated on request, beside those the installed packages reach; sorted."""
        return self._requested

    def use_expand(self):
        """USE_EXPAND's variables, lowercased and sorted, as emerge groups flags by them."""
        return self._use_expand

    def use_expand_hidden(self):
        """USE_EXPAND_HIDDEN's variables, lowercased and sorted: groups emerge leaves out."""
        return self._use_expand_hidden

    def candidates(self, cp=None):
        """Candidates sorted by cp, cpv and repo; only cp's when given."""
        if cp is None:
            return self._candidates
        return tuple(c for c in self._candidates if c.cp == cp)

    def deps(self, cpv, kinds=DEP_KINDS):
        return frozenset(
            edge
            for edge in installed.tree_edges(cpv, self._packages[cpv].deps)
            if edge.kind in kinds
        )


def _iuse(metadata):
    return frozenset(flag.lstrip("+-") for flag in metadata["IUSE"].split())


class EbuildUse:
    """What the profile masks and forces on installed versions' ebuilds.

    read_candidates records what it sees of each ebuild as it passes; the rest go through a
    config of this one's own, which setcpv changes package by package.
    """

    def __init__(self, portdb):
        self._portdb = portdb
        self._settings = None
        # setcpv skips a call whose cpv and metadata id() match its previous call's, so the dict
        # it last saw stays alive until the next call rather than lend its id to another.
        self._current = None
        # (cpv, repo): (IUSE, masked, forced)
        self._states = {}

    def record(self, cpv, repo, iuse, settings):
        """Keeps what settings, set to cpv's ebuild in repo, masks and forces within iuse."""
        self._states[(str(cpv), repo)] = (
            iuse,
            iuse & frozenset(settings.usemask),
            iuse & frozenset(settings.useforce),
        )

    def _set(self, cpv, repo):
        from portage.package.ebuild.config import config

        if self._settings is None:
            self._settings = config(clone=self._portdb.settings)
        metadata = dict(
            zip(
                _CANDIDATE_KEYS,
                self._portdb.aux_get(cpv, list(_CANDIDATE_KEYS), myrepo=repo or None),
            )
        )
        self._settings.setcpv(cpv, mydb=metadata)
        self._current = metadata
        self.record(cpv, repo, _iuse(metadata), self._settings)
        return self._settings

    def _state(self, cpv, repo):
        if (cpv, repo) not in self._states:
            self._set(cpv, repo)
        return self._states[(cpv, repo)]

    def toggles(self, cpv, repo, use):
        """The toggles the user could make on the ebuild: its explicit IUSE, less what the
        profile masks (for a flag that is off) or forces (for a flag that is on)."""
        iuse, masked, forced = self._state(cpv, repo)
        return frozenset(
            {f"-{flag}" for flag in (iuse & use) - forced}
            | {flag for flag in iuse - use - masked}
        )

    def forced(self, cpv, repo, flags):
        """The flags the profile masks or forces on the ebuild, in or out of its IUSE."""
        iuse, masked, forced = self._state(cpv, repo)
        if flags <= iuse:
            return flags & (masked | forced)
        settings = self._set(cpv, repo)
        return flags & (frozenset(settings.usemask) | frozenset(settings.useforce))


def _paren_reduce(depstr):
    # Deprecated in portage, but the only API that keeps the conditionals.
    from portage.dep import paren_reduce

    return paren_reduce(depstr, _deprecation_warn=False)


def _conditionals(tokens, outer, out):
    """Adds each USE conditional's chain of conditionals, outermost first, to out."""
    tokens = iter(tokens)
    for token in tokens:
        if isinstance(token, list):
            _conditionals(token, outer, out)
        elif token.endswith("?"):
            chain = outer + (token[:-1],)
            out.add(chain)
            inner = next(tokens)
            _conditionals(inner if isinstance(inner, list) else [inner], chain, out)


def _toggled(chain, use, toggles):
    """(USE, flags toggled) that make every conditional in chain hold, or None when it holds
    already, needs a toggle outside toggles, or contradicts itself."""
    use = set(use)
    flags = set()
    for condition in chain:
        flag = condition.lstrip("!")
        on = not condition.startswith("!")
        if (flag in use) == on:
            continue
        toggle = flag if on else f"-{flag}"
        if toggle not in toggles:
            return None
        flags.add(toggle)
        if on:
            use.add(flag)
        else:
            use.discard(flag)
    if not flags or any((c.lstrip("!") in use) == c.startswith("!") for c in chain):
        return None
    return frozenset(use), frozenset(flags)


def _flag_order(flag):
    return (flag.lstrip("-"), flag)


def _possible_order(entry):
    return (DEP_KINDS.index(entry.kind), entry.atom, entry.choice, entry.flags)


def possible_dependencies(strings, use, eapi, toggles, match, deps):
    """Possible entries for {kind: dependency string}: per chain of USE conditionals, the atoms
    use_reduce selects as conditional on the flags it takes toggling (its subset), under the
    installed use with them toggled, less what deps (the reduced node tuples) hold already.
    Blockers are constraints, not dependencies, and are left out."""
    found = {}
    for kind, present_nodes in zip(DEP_KINDS, deps):
        present = {node.atom for node in present_nodes}
        chains = set()
        try:
            _conditionals(_paren_reduce(strings[kind]), (), chains)
        except InvalidDependString:
            continue
        # Chains that take the same toggles select the same atoms.
        toggled = {}
        for chain in chains:
            found_toggles = _toggled(chain, use, toggles)
            if found_toggles is not None:
                uselist, flags = found_toggles
                toggled[flags] = uselist
        for flags, uselist in toggled.items():
            subset = {f"!{flag[1:]}" if flag[0] == "-" else flag for flag in flags}
            try:
                tokens = use_reduce(
                    strings[kind],
                    uselist=uselist,
                    eapi=eapi or None,
                    opconvert=False,
                    token_class=Atom,
                    subset=subset,
                )
                selected = installed.nodes(tokens, match)
            except (InvalidAtom, InvalidDependString):
                continue
            for node, choice in zip(selected, installed.choices(selected)):
                if node.type == installed.ATOM and node.atom not in present:
                    key = (kind, node.atom, choice, node.matches)
                    found.setdefault(key, set()).add(flags)
    entries = [
        Possible(kind, atom, choice, matches, tuple(sorted(flags, key=_flag_order)))
        for (kind, atom, choice, matches), sets in found.items()
        for flags in sets
        if not any(other < flags for other in sets)
    ]
    return tuple(sorted(entries, key=_possible_order))


def read_dependencies(vardb, portdb, cpv, match, updates, ebuild_use=None):
    """cpv's Dependencies, without its update; ebuild_use, an EbuildUse, is made here when not
    given."""
    source, strings, eapi = dynamic.dependency_strings(vardb, portdb, cpv, updates)
    use, repo = vardb.aux_get(cpv, ["USE", "repository"])
    use = frozenset(use.split())
    deps, errors = installed.dependency_trees(strings, use, eapi, match)
    possible = ()
    if source == "ebuild":
        toggles = (ebuild_use or EbuildUse(portdb)).toggles(cpv, repo, use)
        possible = possible_dependencies(strings, use, eapi, toggles, match, deps)
    return Dependencies(
        cpv=str(cpv),
        source=dynamic.SOURCES.index(source),
        eapi=eapi,
        errors=tuple(errors),
        deps=deps,
        possible=possible,
    )


def _version(cpv):
    from portage.versions import cpv_getversion

    return cpv_getversion(cpv)


def rebuild_flags(old_use, old_iuse, use, iuse, forced):
    """depgraph's _reinstall_for_flags under --newuse, in emerge's notation (Dependencies)."""
    changed = (old_iuse & old_use) ^ (iuse & use)
    flags = ((old_iuse ^ iuse) - forced) | changed
    found = []
    for flag in sorted(flags):
        star = "*" if flag in changed else ""
        if flag in iuse:
            sign = "" if flag in use else "-"
            added = "" if flag in old_iuse else "%"
            found.append(f"{sign}{flag}{added}{star}")
        else:
            found.append(f"(-{flag}%{star})")
    return tuple(found)


def read_masked(vardb, portdb, settings, cpv, updates):
    """cpv's masked and vdb_masked (Dependencies); settings is a config clone of vardb's."""
    metadata = dict(zip(masks.KEYS, vardb.aux_get(cpv, list(masks.KEYS))))
    vdb_masked = masks.masked(settings, cpv, metadata)
    source, strings, eapi = dynamic.dependency_strings(vardb, portdb, cpv, updates)
    if source != "ebuild":
        return {"masked": vdb_masked, "vdb_masked": vdb_masked}
    # FakeVartree's view: the ebuild's EAPI, KEYWORDS and dependencies.
    (keywords,) = portdb.aux_get(cpv, ["KEYWORDS"], myrepo=metadata["repository"])
    metadata.update(strings, EAPI=eapi, KEYWORDS=keywords)
    return {"masked": masks.masked(settings, cpv, metadata), "vdb_masked": vdb_masked}


def read_update(vardb, cpv, candidates, repositories, ebuild_use):
    """cpv's visible, target and rebuild (Dependencies) against its cp's candidates, as emerge
    -u @installed weighs it; repositories highest priority first, as portdb lists them.
    """
    from portage.versions import vercmp

    slot, use, iuse, repo = vardb.aux_get(cpv, ["SLOT", "USE", "IUSE", "repository"])
    same = [c for c in candidates if c.cpv == cpv]
    # Its own repository's ebuild when there is one, else any.
    visible = any(not c.reasons for c in [c for c in same if c.repo == repo] or same)
    rank = {name: -i for i, name in enumerate(repositories)}
    slot = slot.partition("/")[0]
    best = None
    for c in candidates:
        if c.reasons or c.slot != slot:
            continue
        # Of one version, the ebuild in the repository of highest priority.
        if (
            best is None
            or (
                vercmp(_version(c.cpv), _version(best.cpv))
                or rank[c.repo] - rank[best.repo]
            )
            > 0
        ):
            best = c
    if best is None:
        return {"visible": visible}
    order = vercmp(_version(best.cpv), _version(cpv))
    if order > 0 or not visible:
        return {"visible": visible, "target": (best.cpv, best.repo)}
    if order < 0:
        # Its visible ebuild moved to another slot.
        return {"visible": visible}
    old_use, old_iuse = frozenset(use.split()), _iuse({"IUSE": iuse})
    new_use, new_iuse = frozenset(best.use), frozenset(best.iuse)
    forced = ebuild_use.forced(best.cpv, best.repo, old_iuse ^ new_iuse)
    rebuild = rebuild_flags(old_use, old_iuse, new_use, new_iuse, forced)
    if not rebuild:
        return {"visible": visible}
    return {"visible": visible, "target": (best.cpv, best.repo), "rebuild": rebuild}


def read_candidates(portdb, settings, cp, installed_cpvs, match, ebuild_use=None):
    """Every visible version of cp in every repository, with its dependencies matched through
    match, and each masked one installed; visible as depgraph sees it, which masks an invalid
    ebuild that portdb counts as visible.

    settings is a config clone of portdb's, which this changes package by package; ebuild_use,
    an EbuildUse, records each installed version it sets.
    """
    from portage.dep import Atom
    from portage.package.ebuild.getmaskingstatus import getmaskingstatus

    found = []
    # setcpv skips a call whose cpv and metadata id() match its previous call's, so the dict
    # it last saw stays alive until the next call rather than lend its id to another.
    current = None
    for repo in portdb.getRepositories():
        listed = portdb.cp_list(cp, mytree=portdb.getRepositoryPath(repo))
        if not listed:
            continue
        # Cheaper than asking getmaskingstatus of every version, which only masked ones need.
        visible = set(portdb.xmatch("match-visible", Atom(f"{cp}::{repo}")))
        for cpv in listed:
            if cpv not in visible and cpv not in installed_cpvs:
                continue
            try:
                metadata = dict(
                    zip(
                        _CANDIDATE_KEYS,
                        portdb.aux_get(cpv, list(_CANDIDATE_KEYS), myrepo=repo),
                    )
                )
                reasons = tuple(
                    f"invalid: {message}"
                    for message in masks.invalid_ebuild(portdb, cpv, metadata)
                )
                if reasons and cpv not in installed_cpvs:
                    continue
                if cpv not in visible:
                    found_reasons = getmaskingstatus(
                        cpv, settings=settings, portdb=portdb, myrepo=repo
                    )
                    # Whatever portdb found invisible stays masked, worded or not.
                    reasons += tuple(found_reasons) or ("not visible",)
            except KeyError:
                continue
            settings.setcpv(cpv, mydb=metadata)
            current = metadata
            iuse = _iuse(metadata)
            if ebuild_use is not None and cpv in installed_cpvs:
                ebuild_use.record(cpv, repo, iuse, settings)
            slot, _, sub_slot = metadata["SLOT"].partition("/")
            use = tuple(sorted(settings["PORTAGE_USE"].split()))
            deps, errors, required_use, empty_groups_true = NO_DEPS, (), (), False
            tokens = NO_DEPS
            if not reasons:
                tokens = tuple(tuple(metadata[kind].split()) for kind in DEP_KINDS)
                attrs = _get_eapi_attrs(metadata["EAPI"])
                empty_groups_true = attrs.empty_groups_always_true
                deps, errors = installed.dependency_trees(
                    metadata, frozenset(use), metadata["EAPI"], match
                )
                # An invalid REQUIRED_USE, or one its EAPI lacks, masks it.
                if attrs.required_use:
                    required_use = tuple(metadata["REQUIRED_USE"].split())
            found.append(
                Candidate(
                    cp=cp,
                    cpv=str(cpv),
                    repo=repo,
                    slot=slot,
                    sub_slot=sub_slot or slot,
                    use=use,
                    iuse=tuple(sorted(iuse)),
                    forced=tuple(
                        sorted(
                            iuse
                            & (
                                frozenset(settings.usemask)
                                | frozenset(settings.useforce)
                            )
                        )
                    ),
                    reasons=reasons,
                    errors=tuple(errors),
                    deps=deps,
                    required_use=required_use,
                    empty_groups_true=empty_groups_true,
                    tokens=tokens,
                )
            )
    return found


def reached_cps(deps, may_break=lambda node: False):
    """The cps emerge may have to pull in for node tuples, one per kind: those of the atoms
    nothing installed satisfies, outside any group installed packages satisfy, and every
    alternative of a || group whose satisfied atoms may_break says an update could leave
    unsatisfied. Blockers are not dependencies."""
    found = set()
    for nodes in deps:
        ok = installed.satisfied(nodes)
        children = [[] for _ in nodes]
        for index, node in enumerate(nodes):
            if node.parent >= 0:
                children[node.parent].append(index)

        def atoms(index):
            node = nodes[index]
            if node.type == installed.ATOM:
                yield index
            elif node.type in (installed.ANY_OF, installed.ALL_OF):
                for child in children[index]:
                    yield from atoms(child)

        def open_(index):
            """Whether what installed packages give node index may be taken away."""
            node = nodes[index]
            if not ok[index]:
                return True
            if node.type == installed.ATOM:
                return may_break(node)
            if node.type == installed.ANY_OF:
                return all(open_(child) for child in children[index] if ok[child])
            if node.type == installed.ALL_OF:
                return any(open_(child) for child in children[index])
            return False

        for index, node in enumerate(nodes):
            if node.type == installed.ANY_OF and (
                node.parent < 0 or nodes[node.parent].type != installed.ANY_OF
            ):
                if open_(index):
                    found.update(_cp(nodes[i].atom) for i in atoms(index))
            elif node.type == installed.ATOM and not ok[index]:
                parent = node.parent
                while parent >= 0 and not ok[parent]:
                    parent = nodes[parent].parent
                if parent < 0:
                    found.add(_cp(node.atom))
    return found


def _cp(atom):
    return Atom(atom, allow_repo=True).cp


def may_break(by_cp):
    """may_break for reached_cps: whether an atom node's installed matches all have a visible
    version in their slot, among by_cp's candidates, that the atom rejects (USE aside).
    """
    from portage.dep import match_from_list
    from portage.versions import _pkg_str

    def breaks(node):
        atom = Atom(node.atom, allow_repo=True).without_use
        for cpv in node.matches:
            others = [
                _pkg_str(c.cpv, slot=f"{c.slot}/{c.sub_slot}", repo=c.repo)
                for c in by_cp.get(cpv_getkey(cpv), ())
                if not c.reasons
            ]
            if all(match_from_list(atom, [other]) for other in others):
                return False
        return bool(node.matches)

    return breaks


def build(vardb, portdb, match=None):
    """Evaluate every installed package in vardb against portdb's repositories."""
    return rebuild(vardb, portdb, None, None, match=match)[0]


def rebuild(vardb, portdb, previous, cps, carry=None, match=None, requested=()):
    """(layer, the cps read through portage): as build, but only the cps in cps go through
    portage, every one when cps is None; the other installed packages come from previous, an
    EvaluatedLayer, through carry (unchanged when None), and their cps' candidates with them,
    as do the candidates of the other cps the dependencies still reach. The requested cps
    that are in a repository and not installed are evaluated too, with what they reach.
    """
    from portage.package.ebuild.config import config

    match = match or installed.Matcher(vardb)
    carry = carry or (lambda pkg: pkg)
    cpvs = sorted(str(cpv) for cpv in vardb.cpv_all())
    installed_cps = sorted({cpv_getkey(cpv) for cpv in cpvs})
    previous_candidates = collections.defaultdict(list)
    # The cps previous answered for: its installed ones and those with candidates.
    known = set()
    if previous is not None:
        for c in previous.candidates():
            previous_candidates[c.cp].append(c)
        known.update(previous_candidates)
        known.update(cpv_getkey(cpv) for cpv in previous.installed())

    def fresh(cp):
        return cps is None or cp in cps or cp not in known

    packages = []
    if previous is not None:
        packages.extend(
            carry(previous.package(cpv)) for cpv in cpvs if not fresh(cpv_getkey(cpv))
        )
    evaluate = [cpv for cpv in cpvs if fresh(cpv_getkey(cpv))]
    read = set()
    by_cp = {}
    settings = config(clone=portdb.settings)
    installed_cpvs = frozenset(cpvs)
    # Candidates first: they set a config to every installed ebuild the toggles need.
    ebuild_use = EbuildUse(portdb)

    def candidates_of(cp):
        if fresh(cp):
            read.add(cp)
            return read_candidates(
                portdb, settings, cp, installed_cpvs, match, ebuild_use
            )
        return [carry(c) for c in previous_candidates[cp]]

    for cp in installed_cps:
        by_cp[cp] = candidates_of(cp)
    if evaluate:
        updates = dynamic.global_updates(portdb)
        repositories = portdb.getRepositories()
        installed_settings = config(clone=vardb.settings)
        versions = collections.Counter(cpv_getkey(cpv) for cpv in cpvs)
        for cpv in evaluate:
            cp = cpv_getkey(cpv)
            pkg = read_dependencies(
                vardb, portdb, cpv, match, updates, ebuild_use
            )._replace(**read_update(vardb, cpv, by_cp[cp], repositories, ebuild_use))
            # Masks are costly (the license check above all), and depclean only weighs them
            # for a package that is not visible or shares its cp with another installed
            # version.
            if not pkg.visible or versions[cp] > 1:
                pkg = pkg._replace(
                    **read_masked(vardb, portdb, installed_settings, cpv, updates)
                )
            packages.append(pkg)
    repository_cps = portdb.cp_all()
    requested = (
        frozenset(requested).intersection(repository_cps).difference(installed_cps)
    )
    # What emerge may have to pull in (reached_cps), and theirs in turn.
    reached = set(installed_cps)
    breaks = may_break(by_cp)
    queue = sorted(requested)
    for deps in itertools.chain(
        (pkg.deps for pkg in packages),
        (c.deps for found in by_cp.values() for c in found),
    ):
        queue.extend(reached_cps(deps, breaks) - reached)
    while queue:
        cp = queue.pop()
        if cp in reached:
            continue
        reached.add(cp)
        by_cp[cp] = candidates_of(cp)
        for c in by_cp[cp]:
            queue.extend(reached_cps(c.deps, breaks) - reached)
    return (
        EvaluatedLayer(
            packages,
            [c for found in by_cp.values() for c in found],
            repository_cps,
            requested,
            portdb.settings.get("USE_EXPAND", "").lower().split(),
            portdb.settings.get("USE_EXPAND_HIDDEN", "").lower().split(),
        ),
        frozenset(read),
    )


def to_json(layer):
    """The layer as canonical JSON: sorted keys, packages by cpv, candidates as stored."""
    packages = [
        {
            "cpv": pkg.cpv,
            "source": dynamic.SOURCES[pkg.source],
            "eapi": pkg.eapi,
            "errors": [list(error) for error in pkg.errors],
            "deps": installed.deps_json(pkg.deps),
            "possible": [
                {
                    "kind": p.kind,
                    "atom": p.atom,
                    "choice": p.choice,
                    "matches": list(p.matches),
                    "flags": list(p.flags),
                }
                for p in pkg.possible
            ],
            "visible": pkg.visible,
            "masked": pkg.masked,
            "vdb_masked": pkg.vdb_masked,
            "target": (
                None
                if pkg.target is None
                else {"cpv": pkg.target[0], "repo": pkg.target[1]}
            ),
            "rebuild": list(pkg.rebuild),
        }
        for pkg in layer
    ]
    candidates = [
        {
            "cp": c.cp,
            "cpv": c.cpv,
            "repo": c.repo,
            "slot": c.slot,
            "sub_slot": c.sub_slot,
            "use": list(c.use),
            "iuse": list(c.iuse),
            "forced": list(c.forced),
            "reasons": list(c.reasons),
            "errors": [list(error) for error in c.errors],
            "deps": installed.deps_json(c.deps),
            "required_use": list(c.required_use),
            "empty_groups_true": c.empty_groups_true,
            "tokens": [list(kind) for kind in c.tokens],
        }
        for c in layer.candidates()
    ]
    document = {
        "format": 8,
        "packages": packages,
        "candidates": candidates,
        "repository_cps": list(layer.repository_cps()),
        "requested": list(layer.requested()),
        "use_expand": list(layer.use_expand()),
        "use_expand_hidden": list(layer.use_expand_hidden()),
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
