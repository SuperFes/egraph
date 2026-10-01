"""The installed layer: installed packages, their USE-reduced dependency trees,
and the installed packages each atom resolves to.

It answers the same queries as egraph_build.oracle, with the vardb argument
dropped, from an index built in one pass. Portage does all the evaluation:
use_reduce shapes the trees and vardb.match resolves every atom, so USE
dependencies, their defaults and slot operators mean exactly what they mean
to portage.
"""

import collections
import functools
import json
from typing import NamedTuple

from portage.dep import Atom, use_reduce
from portage.dep.soname.parse import parse_soname_deps
from portage.exception import InvalidAtom, InvalidData, InvalidDependString
from portage.versions import cpv_getkey

from egraph_build.model import DEP_KINDS, Edge, SonameUse
from egraph_build.roots import read_roots

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


class Matcher:
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


def nodes(tokens, match):
    """The node tuple for use_reduce's tokens, atoms resolved through match."""
    found = []
    _tree(tokens, -1, found, match)
    return tuple(found)


def dependency_trees(strings, use, eapi, match):
    """(node tuples per kind, errors) for {kind: dependency string} reduced under use."""
    errors = []
    deps = []
    for kind in DEP_KINDS:
        try:
            tokens = use_reduce(
                strings[kind],
                uselist=use,
                eapi=eapi or None,
                opconvert=False,
                token_class=Atom,
            )
            deps.append(nodes(tokens, match))
        except (InvalidAtom, InvalidDependString) as e:
            errors.append((kind, str(e)))
            deps.append(())
    return tuple(deps), errors


def read_package(vardb, cpv, match):
    metadata = dict(zip(_AUX_KEYS, vardb.aux_get(cpv, _AUX_KEYS)))
    use = frozenset(metadata["USE"].split())
    slot, _, sub_slot = metadata["SLOT"].partition("/")
    deps, errors = dependency_trees(metadata, use, metadata["EAPI"], match)
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
        deps=deps,
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


def tree_edges(cpv, deps):
    """The Edges of a package's node tuples, one per kind; blockers are not dependencies."""
    edges = set()
    for kind, nodes in zip(DEP_KINDS, deps):
        for node, choice in zip(nodes, choices(nodes)):
            if node.type == ATOM:
                edges.update(
                    Edge(cpv, child, kind, node.atom, choice) for child in node.matches
                )
    return frozenset(edges)


class _Index(NamedTuple):
    edges: dict
    reverse: dict
    matches: dict
    providers: dict
    consumers: dict


class InstalledLayer:
    def __init__(self, packages, roots=()):
        self._packages = {pkg.cpv: pkg for pkg in sorted(packages, key=_cpv)}
        self._roots = tuple(roots)

    # Built on first query: building and encoding a layer never need it.
    @functools.cached_property
    def _index(self):
        index = _Index({}, collections.defaultdict(set), {}, {}, {})
        providers = collections.defaultdict(set)
        consumers = collections.defaultdict(set)
        for pkg in self._packages.values():
            for nodes in pkg.deps:
                for node in nodes:
                    if node.type == ATOM:
                        index.matches[node.atom] = node.matches
            edges = tree_edges(pkg.cpv, pkg.deps)
            index.edges[pkg.cpv] = edges
            for edge in edges:
                index.reverse[edge.child].add(edge)
            for category, soname in pkg.provides:
                providers[soname].add(SonameUse(pkg.cpv, category))
            for category, soname in pkg.requires:
                consumers[soname].add(SonameUse(pkg.cpv, category))
        index.providers.update(providers)
        index.consumers.update(consumers)
        return index

    def __iter__(self):
        return iter(self._packages.values())

    def package(self, cpv):
        return self._packages[cpv]

    def installed(self):
        return tuple(self._packages)

    def roots(self):
        """egraph_build.roots.Root records, sets in ROOT_SETS order and atoms sorted."""
        return self._roots

    def matches(self, atom):
        """Installed cpvs an atom from any installed dependency tree resolves to."""
        return self._index.matches[atom]

    def errors(self):
        return frozenset(
            (pkg.cpv, key) for pkg in self._packages.values() for key, _ in pkg.errors
        )

    def deps(self, cpv, kinds=DEP_KINDS):
        return frozenset(edge for edge in self._index.edges[cpv] if edge.kind in kinds)

    def rdeps(self, cpv, kinds=DEP_KINDS):
        return frozenset(
            edge for edge in self._index.reverse.get(cpv, ()) if edge.kind in kinds
        )

    def soname_providers(self, soname):
        return frozenset(self._index.providers.get(soname, ()))

    def soname_consumers(self, soname):
        return frozenset(self._index.consumers.get(soname, ()))


def _cpv(pkg):
    return pkg.cpv


def build(vardb):
    """Evaluate every installed package in vardb into an InstalledLayer."""
    match = Matcher(vardb)
    return InstalledLayer(
        (read_package(vardb, cpv, match) for cpv in vardb.cpv_all()),
        read_roots(vardb, match),
    )


def deps_json(deps):
    """A package's node tuples, one per kind, as to_json writes them."""
    return {
        kind: [
            {
                "type": NODE_TYPES[node.type],
                "parent": node.parent,
                "atom": node.atom,
                "matches": list(node.matches),
            }
            for node in nodes
        ]
        for kind, nodes in zip(DEP_KINDS, deps)
    }


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
                "deps": deps_json(pkg.deps),
                "provides": [list(soname) for soname in pkg.provides],
                "requires": [list(soname) for soname in pkg.requires],
            }
        )
    roots = [
        {
            "set": root.set,
            "atom": root.atom,
            "matches": list(root.matches),
            "via": root.via,
        }
        for root in layer.roots()
    ]
    document = {"format": 3, "packages": packages, "roots": roots}
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
