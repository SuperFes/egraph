"""The evaluated layer: the installed packages as emerge sees them against the repositories.

Per installed package, its dependency trees under emerge's default --dynamic-deps=y, matched
against the installed packages, and what its ebuild would add with flags toggled; per installed
cp, the versions it could move to, with the USE each would be built with now. Portage does all
the evaluation, as for the installed layer.
"""

import json
from typing import NamedTuple

from portage.dep import Atom, use_reduce
from portage.exception import InvalidAtom, InvalidDependString
from portage.versions import cpv_getkey

from egraph_build import dynamic, installed
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


class Candidate(NamedTuple):
    """One version of an installed cp in one repository."""

    cp: str
    cpv: str
    repo: str
    slot: str
    sub_slot: str
    # The flags the ebuild would be built with now, within its IUSE.
    use: tuple
    # Without + and - defaults.
    iuse: tuple
    # Why it is masked, as portage words it; empty when it is visible.
    reasons: tuple


def _cpv(pkg):
    return pkg.cpv


def _candidate_key(candidate):
    return (candidate.cp, candidate.cpv, candidate.repo)


class EvaluatedLayer:
    def __init__(self, packages, candidates):
        self._packages = {pkg.cpv: pkg for pkg in sorted(packages, key=_cpv)}
        self._candidates = tuple(sorted(candidates, key=_candidate_key))

    def __iter__(self):
        return iter(self._packages.values())

    def package(self, cpv):
        return self._packages[cpv]

    def installed(self):
        return tuple(self._packages)

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


class UseToggles:
    """The toggles the user could make on an ebuild: its explicit IUSE, less what the profile
    masks (for a flag that is off) or forces (for a flag that is on).

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

    def __call__(self, cpv, repo, use):
        states = self._states.get((cpv, repo))
        if states is None:
            from portage.package.ebuild.config import config

            if self._settings is None:
                self._settings = config(clone=self._portdb.settings)
            metadata = dict(
                zip(
                    _CANDIDATE_KEYS,
                    self._portdb.aux_get(
                        cpv, list(_CANDIDATE_KEYS), myrepo=repo or None
                    ),
                )
            )
            self._settings.setcpv(cpv, mydb=metadata)
            self._current = metadata
            self.record(cpv, repo, _iuse(metadata), self._settings)
            states = self._states[(cpv, repo)]
        iuse, masked, forced = states
        return frozenset(
            {f"-{flag}" for flag in (iuse & use) - forced}
            | {flag for flag in iuse - use - masked}
        )


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


def read_dependencies(vardb, portdb, cpv, match, updates, use_toggles=None):
    """cpv's Dependencies; use_toggles, a UseToggles, is made here when not given."""
    source, strings, eapi = dynamic.dependency_strings(vardb, portdb, cpv, updates)
    use, repo = vardb.aux_get(cpv, ["USE", "repository"])
    use = frozenset(use.split())
    deps, errors = installed.dependency_trees(strings, use, eapi, match)
    possible = ()
    if source == "ebuild":
        toggles = (use_toggles or UseToggles(portdb))(cpv, repo, use)
        possible = possible_dependencies(strings, use, eapi, toggles, match, deps)
    return Dependencies(
        cpv=str(cpv),
        source=dynamic.SOURCES.index(source),
        eapi=eapi,
        errors=tuple(errors),
        deps=deps,
        possible=possible,
    )


def read_candidates(portdb, settings, cp, installed_cpvs, use_toggles=None):
    """Every visible version of cp in every repository, and each masked one installed.

    settings is a config clone of portdb's, which this changes package by package; use_toggles,
    a UseToggles, records each installed version it sets.
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
                reasons = ()
                if cpv not in visible:
                    found_reasons = getmaskingstatus(
                        cpv, settings=settings, portdb=portdb, myrepo=repo
                    )
                    # Whatever portdb found invisible stays masked, worded or not.
                    reasons = tuple(found_reasons) or ("not visible",)
            except KeyError:
                continue
            settings.setcpv(cpv, mydb=metadata)
            current = metadata
            iuse = _iuse(metadata)
            if use_toggles is not None and cpv in installed_cpvs:
                use_toggles.record(cpv, repo, iuse, settings)
            slot, _, sub_slot = metadata["SLOT"].partition("/")
            found.append(
                Candidate(
                    cp=cp,
                    cpv=str(cpv),
                    repo=repo,
                    slot=slot,
                    sub_slot=sub_slot or slot,
                    use=tuple(sorted(settings["PORTAGE_USE"].split())),
                    iuse=tuple(sorted(iuse)),
                    reasons=reasons,
                )
            )
    return found


def build(vardb, portdb, match=None):
    """Evaluate every installed package in vardb against portdb's repositories."""
    from portage.package.ebuild.config import config

    match = match or installed.Matcher(vardb)
    cpvs = sorted(str(cpv) for cpv in vardb.cpv_all())
    updates = dynamic.global_updates(portdb)
    settings = config(clone=portdb.settings)
    installed_cpvs = frozenset(cpvs)
    # Candidates first: they set a config to every installed ebuild the toggles need.
    use_toggles = UseToggles(portdb)
    candidates = []
    for cp in sorted({cpv_getkey(cpv) for cpv in cpvs}):
        candidates.extend(
            read_candidates(portdb, settings, cp, installed_cpvs, use_toggles)
        )
    packages = [
        read_dependencies(vardb, portdb, cpv, match, updates, use_toggles)
        for cpv in cpvs
    ]
    return EvaluatedLayer(packages, candidates)


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
            "reasons": list(c.reasons),
        }
        for c in layer.candidates()
    ]
    document = {"format": 2, "packages": packages, "candidates": candidates}
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
