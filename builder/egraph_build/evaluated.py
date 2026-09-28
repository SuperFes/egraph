"""The evaluated layer: the installed packages as emerge sees them against the repositories.

Per installed package, its dependency trees under emerge's default --dynamic-deps=y, matched
against the installed packages; per installed cp, the versions it could move to, with the USE
each would be built with now. Portage does all the evaluation, as for the installed layer.
"""

import json
from typing import NamedTuple

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


def read_dependencies(vardb, portdb, cpv, match, updates):
    source, strings, eapi = dynamic.dependency_strings(vardb, portdb, cpv, updates)
    (use,) = vardb.aux_get(cpv, ["USE"])
    deps, errors = installed.dependency_trees(
        strings, frozenset(use.split()), eapi, match
    )
    return Dependencies(
        cpv=str(cpv),
        source=dynamic.SOURCES.index(source),
        eapi=eapi,
        errors=tuple(errors),
        deps=deps,
    )


def read_candidates(portdb, settings, cp, installed_cpvs):
    """Every visible version of cp in every repository, and each masked one installed.

    settings is a config clone of portdb's, which this changes package by package.
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
            slot, _, sub_slot = metadata["SLOT"].partition("/")
            found.append(
                Candidate(
                    cp=cp,
                    cpv=str(cpv),
                    repo=repo,
                    slot=slot,
                    sub_slot=sub_slot or slot,
                    use=tuple(sorted(settings["PORTAGE_USE"].split())),
                    iuse=tuple(
                        sorted({flag.lstrip("+-") for flag in metadata["IUSE"].split()})
                    ),
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
    packages = [read_dependencies(vardb, portdb, cpv, match, updates) for cpv in cpvs]
    settings = config(clone=portdb.settings)
    installed_cpvs = frozenset(cpvs)
    candidates = []
    for cp in sorted({cpv_getkey(cpv) for cpv in cpvs}):
        candidates.extend(read_candidates(portdb, settings, cp, installed_cpvs))
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
    document = {"format": 1, "packages": packages, "candidates": candidates}
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
