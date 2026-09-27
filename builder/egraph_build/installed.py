"""The installed layer: installed packages, their USE-reduced dependency trees,
and the installed packages each atom resolves to.

It answers the same queries as egraph_build.oracle, with the vardb argument
dropped, from an index built in one pass. Portage does all the evaluation:
use_reduce shapes the trees and vardb.match resolves every atom, so USE
dependencies, their defaults and slot operators mean exactly what they mean
to portage.
"""

import collections
import json
from typing import NamedTuple

from portage.dep import Atom, use_reduce
from portage.dep.soname.parse import parse_soname_deps
from portage.exception import InvalidAtom, InvalidData, InvalidDependString
from portage.versions import cpv_getkey

from egraph_build.model import DEP_KINDS, Edge, SonameUse

ATOM, ANY_OF, ALL_OF, WEAK_BLOCKER, STRONG_BLOCKER = range(5)
NODE_TYPES = ("atom", "any-of", "all-of", "weak-blocker", "strong-blocker")
SONAME_KEYS = ("PROVIDES", "REQUIRES")
_AUX_KEYS = DEP_KINDS + SONAME_KEYS + ("EAPI", "IUSE", "SLOT", "USE", "repository")


class Node(NamedTuple):
    type: int
    # Index of the enclosing any-of or all-of node in the same list, or -1 at top level.
    parent: int
    # The atom as portage prints it after USE reduction; empty for groups.
    atom: str
    # Installed cpvs the atom matches with USE deps honored; for a blocker, what it blocks.
    matches: tuple


class Package(NamedTuple):
    cpv: str
    cp: str
    slot: str
    sub_slot: str
    repo: str
    eapi: str
    use: tuple
    iuse: tuple
    # (key, message) for every dependency or soname string portage could not parse.
    errors: tuple
    # One node tuple per kind, in DEP_KINDS order. Children follow their parent.
    deps: tuple
    # (multilib category, soname)
    provides: tuple
    requires: tuple


class _Matcher:
    """vardb.match per distinct atom string; 36k atoms on a real system are 5.7k distinct."""

    def __init__(self, vardb):
        self._vardb = vardb
        self._cache = {}

    def __call__(self, atom):
        key = str(atom)
        found = self._cache.get(key)
        if found is None:
            found = self._cache[key] = tuple(
                sorted(str(cpv) for cpv in self._vardb.match(atom))
            )
        return found


def _tree(tokens, parent, nodes, match):
    tokens = iter(tokens)
    for token in tokens:
        if token == "||":
            index = len(nodes)
            nodes.append(Node(ANY_OF, parent, "", ()))
            _tree(next(tokens), index, nodes, match)
        elif isinstance(token, list):
            index = len(nodes)
            nodes.append(Node(ALL_OF, parent, "", ()))
            _tree(token, index, nodes, match)
        elif token.blocker:
            node_type = STRONG_BLOCKER if token.blocker.overlap.forbid else WEAK_BLOCKER
            blocked = match(Atom(str(token).lstrip("!")))
            nodes.append(Node(node_type, parent, str(token), blocked))
        else:
            nodes.append(Node(ATOM, parent, str(token), match(token)))


def read_package(vardb, cpv, match):
    metadata = dict(zip(_AUX_KEYS, vardb.aux_get(cpv, _AUX_KEYS)))
    use = frozenset(metadata["USE"].split())
    slot, _, sub_slot = metadata["SLOT"].partition("/")
    errors = []
    deps = []
    for kind in DEP_KINDS:
        nodes = []
        try:
            tokens = use_reduce(
                metadata[kind],
                uselist=use,
                eapi=metadata["EAPI"] or None,
                opconvert=False,
                token_class=Atom,
            )
            _tree(tokens, -1, nodes, match)
        except (InvalidAtom, InvalidDependString) as e:
            errors.append((kind, str(e)))
            nodes = []
        deps.append(tuple(nodes))
    sonames = {}
    for key in SONAME_KEYS:
        try:
            sonames[key] = tuple(
                (atom.multilib_category, atom.soname)
                for atom in parse_soname_deps(metadata[key])
            )
        except InvalidData as e:
            errors.append((key, str(e)))
            sonames[key] = ()
    return Package(
        cpv=str(cpv),
        cp=cpv_getkey(cpv),
        slot=slot,
        sub_slot=sub_slot or slot,
        repo=metadata["repository"],
        eapi=metadata["EAPI"],
        use=tuple(sorted(use)),
        iuse=tuple(sorted(metadata["IUSE"].split())),
        errors=tuple(errors),
        deps=tuple(deps),
        provides=sonames["PROVIDES"],
        requires=sonames["REQUIRES"],
    )


def choices(nodes):
    """Per node, whether it sits inside a || group."""
    inside = []
    for node in nodes:
        parent = node.parent
        inside.append(parent >= 0 and (nodes[parent].type == ANY_OF or inside[parent]))
    return tuple(inside)


def satisfied(nodes):
    """Per node, whether installed packages satisfy it.

    Blockers are constraints, not dependencies, so they never make a tree
    unsatisfied. An empty any-of can only come from EAPIs where portage
    treats it as satisfied; later EAPIs get a never-matching atom instead.
    """
    children = [[] for _ in nodes]
    for index, node in enumerate(nodes):
        if node.parent >= 0:
            children[node.parent].append(index)
    result = [True] * len(nodes)
    # Children always follow their parent, so one backwards pass sees them first.
    for index in reversed(range(len(nodes))):
        node = nodes[index]
        if node.type == ATOM:
            result[index] = bool(node.matches)
        elif node.type == ANY_OF:
            kids = children[index]
            result[index] = not kids or any(result[kid] for kid in kids)
        elif node.type == ALL_OF:
            result[index] = all(result[kid] for kid in children[index])
    return tuple(result)


class InstalledLayer:
    def __init__(self, packages):
        self._packages = {pkg.cpv: pkg for pkg in sorted(packages)}
        self._edges = {}
        self._reverse = collections.defaultdict(set)
        self._matches = {}
        self._providers = collections.defaultdict(set)
        self._consumers = collections.defaultdict(set)
        for pkg in self._packages.values():
            edges = set()
            for kind, nodes in zip(DEP_KINDS, pkg.deps):
                for node, choice in zip(nodes, choices(nodes)):
                    if node.type != ATOM:
                        continue
                    self._matches[node.atom] = node.matches
                    for child in node.matches:
                        edges.add(Edge(pkg.cpv, child, kind, node.atom, choice))
            self._edges[pkg.cpv] = frozenset(edges)
            for edge in edges:
                self._reverse[edge.child].add(edge)
            for category, soname in pkg.provides:
                self._providers[soname].add(SonameUse(pkg.cpv, category))
            for category, soname in pkg.requires:
                self._consumers[soname].add(SonameUse(pkg.cpv, category))

    def __iter__(self):
        return iter(self._packages.values())

    def package(self, cpv):
        return self._packages[cpv]

    def installed(self):
        return tuple(self._packages)

    def matches(self, atom):
        """Installed cpvs an atom from any installed dependency tree resolves to."""
        return self._matches[atom]

    def errors(self):
        return frozenset(
            (pkg.cpv, key) for pkg in self._packages.values() for key, _ in pkg.errors
        )

    def deps(self, cpv, kinds=DEP_KINDS):
        return frozenset(edge for edge in self._edges[cpv] if edge.kind in kinds)

    def rdeps(self, cpv, kinds=DEP_KINDS):
        return frozenset(
            edge for edge in self._reverse.get(cpv, ()) if edge.kind in kinds
        )

    def soname_providers(self, soname):
        return frozenset(self._providers.get(soname, ()))

    def soname_consumers(self, soname):
        return frozenset(self._consumers.get(soname, ()))


def build(vardb):
    """Evaluate every installed package in vardb into an InstalledLayer."""
    match = _Matcher(vardb)
    return InstalledLayer(read_package(vardb, cpv, match) for cpv in vardb.cpv_all())


def to_json(layer):
    """The layer as canonical JSON: sorted keys and packages, trees in node order."""
    packages = []
    for pkg in layer:
        packages.append(
            {
                "cpv": pkg.cpv,
                "cp": pkg.cp,
                "slot": pkg.slot,
                "sub_slot": pkg.sub_slot,
                "repo": pkg.repo,
                "eapi": pkg.eapi,
                "use": list(pkg.use),
                "iuse": list(pkg.iuse),
                "errors": [list(error) for error in pkg.errors],
                "deps": {
                    kind: [
                        {
                            "type": NODE_TYPES[node.type],
                            "parent": node.parent,
                            "atom": node.atom,
                            "matches": list(node.matches),
                        }
                        for node in nodes
                    ]
                    for kind, nodes in zip(DEP_KINDS, pkg.deps)
                },
                "provides": [list(soname) for soname in pkg.provides],
                "requires": [list(soname) for soname in pkg.requires],
            }
        )
    document = {"format": 1, "packages": packages}
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
