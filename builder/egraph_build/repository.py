"""The repository index: every version in the repositories, and what portage decides their
visibility from, its metadata and the visibility ledger (docs/store-format.md), and the main
repository's security advisories. egraph evaluates the visibility, and which advisories affect
the system, itself.
"""

import json
import os
from typing import NamedTuple

from egraph_build import binaries, ebuild, evaluated, ledger, masks
from egraph_build.ledger import VisibilityLedger

# What a version record holds of the ebuild's metadata, and what its validity is checked on.
KEYS = tuple(
    sorted(
        {
            *masks.EBUILD_KEYS,
            "DESCRIPTION",
            "EAPI",
            "HOMEPAGE",
            "INHERITED",
            "KEYWORDS",
            "LICENSE",
            "PROPERTIES",
            "RESTRICT",
            "SLOT",
            "SRC_URI",
        }
    )
)


class FetchGroup(NamedTuple):
    """The files SRC_URI names in the same places, and their bytes: fetched once when the
    conditionals over any one of those places are all met."""

    # Per place, its conditionals, "flag?" or "!flag?", the outermost first.
    places: tuple
    bytes: int


class Version(NamedTuple):
    cp: str
    cpv: str
    repo: str
    slot: str
    sub_slot: str
    eapi: str
    keywords: tuple
    license: tuple
    properties: tuple
    restrict: tuple
    # The flags enabled that LICENSE's, PROPERTIES's or RESTRICT's conditionals test, where
    # LICENSE or PROPERTIES has one, as portdbapi's _visible reads its USE only then.
    use: tuple
    description: str
    homepage: str
    # What depgraph finds invalid in it, as it words each after "invalid: ".
    invalid: tuple
    # The eclasses it inherits, as INHERITED lists them.
    inherited: tuple = ()
    # Its distfiles, FetchGroups in SRC_URI's order.
    fetch: tuple = ()


class Eapi(NamedTuple):
    eapi: str
    supported: bool
    deprecated: bool


class Visibility(NamedTuple):
    """What the visibility ledger leaves to portage itself."""

    eapis: tuple
    arch: str


class Repository(NamedTuple):
    name: str
    location: str
    # Whether emerge --search reads its descriptions from a metadata/pkg_desc_index.
    description_index: bool


class AdvisoryPackage(NamedTuple):
    """A package entry of a GLSA: its ranges as portage's glsa module makes them atoms."""

    cp: str
    # "*" or the keywords it applies to, space-separated.
    arch: str
    vulnerable: tuple
    unaffected: tuple


class Advisory(NamedTuple):
    id: str
    title: str
    synopsis: str
    # The <revised> count, which a change to it raises.
    revision: int
    packages: tuple


class RepositoryIndex(NamedTuple):
    # Repository records, in portage's order: the highest priority first.
    repositories: tuple
    versions: tuple
    visibility: Visibility
    # Sorted by id.
    advisories: tuple = ()
    # Where each source of visibility was read from, entry by entry.
    ledger: VisibilityLedger = VisibilityLedger()
    # The binary packages' sources, PKGDIR first, and the binaries, sorted by cpv, source and
    # build id.
    binary_sources: tuple = ()
    binaries: tuple = ()


def _tokens(text):
    return tuple(text.split())


def _tested(tokens):
    return {token.lstrip("!")[:-1] for token in tokens if token.endswith("?")}


class UseReader:
    """A config clone made on first use, for the USE of the few versions that need it."""

    def __init__(self, portdb):
        self._portdb = portdb
        self._settings = None
        # The package setcpv last saw, kept alive (see ebuild.set_ebuild).
        self._current = None

    def use(self, cpv, repo):
        """The USE the ebuild would be built with now, as portdbapi's _visible takes it."""
        import portage

        if self._settings is None:
            self._settings = portage.config(clone=self._portdb.settings)
        keys = evaluated._CANDIDATE_KEYS
        try:
            metadata = dict(
                zip(keys, self._portdb.aux_get(cpv, list(keys), myrepo=repo))
            )
        except KeyError:
            return set()
        self._current = ebuild.set_ebuild(self._settings, cpv, metadata)
        return set(self._settings["PORTAGE_USE"].split())


def with_use(version, settings):
    """version with the USE its LICENSE, PROPERTIES and RESTRICT conditionals test, where
    LICENSE or PROPERTIES has one."""
    if not _tested(version.license + version.properties):
        return version._replace(use=())
    tested = _tested(version.license + version.properties + version.restrict)
    use = settings.use(version.cpv, version.repo) & tested
    return version._replace(use=tuple(sorted(use)))


def fetch_groups(src_uri, sizes):
    """SRC_URI's files grouped by the places it names them in, each place the conditionals over
    it, as portage's fetch map names a file (a URI's basename, or what "->" renames it to);
    groups in the order each first appears, each file's bytes from sizes, a file it lacks
    counting none, and groups of no bytes left out."""
    tokens = src_uri.split()
    # By file, the conditionals over each place it appears, in order.
    places = {}
    # The conditional over each open parenthesis, None for a plain one.
    stack = []
    pending = None
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token.endswith("?"):
            pending = token
        elif token == "(":
            stack.append(pending)
            pending = None
        elif token == ")":
            if stack:
                stack.pop()
        else:
            name = os.path.basename(token)
            if i + 2 < len(tokens) and tokens[i + 1] == "->":
                name = tokens[i + 2]
                i += 2
            conditions = tuple(c for c in stack if c is not None)
            found = places.setdefault(name, [])
            if conditions not in found:
                found.append(conditions)
        i += 1
    groups = {}
    for name, found in places.items():
        key = tuple(found)
        groups[key] = groups.get(key, 0) + sizes.get(name, 0)
    return tuple(FetchGroup(p, b) for p, b in groups.items() if b)


def distfile_sizes(portdb, cp, repo):
    """By distfile name, its bytes as the Manifest of cp's directory in repo records them;
    nothing where it has none."""
    from portage.exception import PortageException

    location = portdb.getRepositoryPath(repo)
    try:
        manifest = portdb.repositories.get_repo_for_location(location).load_manifest(
            os.path.join(location, cp), portdb.settings["DISTDIR"]
        )
    except (OSError, PortageException):
        return {}
    return {
        name: int(entry["size"])
        for name, entry in manifest.fhashdict.get("DIST", {}).items()
        if "size" in entry
    }


def read_versions(portdb, cps, settings=None):
    """The version records of cps in every repository, in the index's order."""
    settings = settings or UseReader(portdb)
    found = []
    for cp in sorted(cps):
        for repo in portdb.getRepositories():
            sizes = None
            for cpv in portdb.cp_list(cp, mytree=portdb.getRepositoryPath(repo)):
                try:
                    metadata = dict(
                        zip(KEYS, portdb.aux_get(cpv, list(KEYS), myrepo=repo))
                    )
                except KeyError:
                    continue
                metadata["repository"] = repo
                if sizes is None and metadata["SRC_URI"]:
                    sizes = distfile_sizes(portdb, cp, repo)
                slot, _, sub_slot = metadata["SLOT"].partition("/")
                version = Version(
                    cp=cp,
                    cpv=str(cpv),
                    repo=repo,
                    slot=slot,
                    sub_slot=sub_slot or slot,
                    eapi=metadata["EAPI"],
                    keywords=_tokens(metadata["KEYWORDS"]),
                    license=_tokens(metadata["LICENSE"]),
                    properties=_tokens(metadata["PROPERTIES"]),
                    restrict=_tokens(metadata["RESTRICT"]),
                    use=(),
                    description=metadata["DESCRIPTION"],
                    homepage=metadata["HOMEPAGE"],
                    invalid=tuple(masks.invalid_ebuild(portdb, cpv, metadata)),
                    inherited=_tokens(metadata["INHERITED"]),
                    fetch=fetch_groups(metadata["SRC_URI"], sizes or {}),
                )
                found.append(with_use(version, settings))
    return tuple(found)


def _description_index(portdb, location):
    """Whether IndexedPortdb finds a pkg_desc_index for the repository at location."""
    outside = os.path.join(portdb.depcachedir, location.lstrip(os.sep))
    return any(
        os.path.exists(os.path.join(parent, "metadata", "pkg_desc_index"))
        for parent in (location, outside)
    )


def repositories(portdb):
    found = []
    for name in portdb.getRepositories():
        location = portdb.getRepositoryPath(name)
        found.append(Repository(name, location, _description_index(portdb, location)))
    return tuple(found)


def advisory_directory(settings):
    """Where portage's glsa module reads the GLSAs from; None without a main repository."""
    if "GLSA_DIR" in settings:
        return settings["GLSA_DIR"]
    if not settings.get("PORTDIR"):
        return None
    return os.path.join(settings["PORTDIR"], "metadata", "glsa")


def read_advisories(settings):
    """The GLSAs portage's glsa module reads, as it parses them, sorted by id. Those it cannot
    parse, or whose arch it would refuse to test, are left out, as glsa-check skips the first.
    """
    from portage import glsa

    if advisory_directory(settings) is None:
        return ()
    found = []
    for nr in sorted(glsa.get_glsa_list(settings)):
        try:
            advisory = glsa.Glsa(nr, settings, None, None)
        except Exception:
            # Anything from a malformed XML file to a bad atom: glsa-check gives up on it too.
            continue
        packages = tuple(
            AdvisoryPackage(
                str(cp),
                path["arch"],
                tuple(atom.strip() for atom in path["vul_atoms"]),
                tuple(atom.strip() for atom in path["unaff_atoms"]),
            )
            for cp, paths in advisory.packages.items()
            for path in paths
        )
        if not all(glsa.ARCH_REGEX.match(p.arch) for p in packages):
            continue
        found.append(
            Advisory(nr, advisory.title, advisory.synopsis, advisory.count, packages)
        )
    return tuple(found)


def assemble(portdb, versions, advisories=None, binary=None):
    """The index of versions, with the repositories, the visibility configuration and, unless
    given, the advisories and the (sources, binaries) now."""
    sources, found = binaries.read(portdb) if binary is None else binary
    return RepositoryIndex(
        repositories=repositories(portdb),
        versions=tuple(versions),
        visibility=read_visibility(portdb.settings, {v.eapi for v in versions}),
        advisories=(
            read_advisories(portdb.settings) if advisories is None else advisories
        ),
        ledger=ledger.read_visibility(portdb.settings),
        binary_sources=sources,
        binaries=found,
    )


def read_visibility(settings, eapis):
    from portage import _eapi_is_deprecated, eapi_is_supported

    return Visibility(
        eapis=tuple(
            Eapi(eapi, bool(eapi_is_supported(eapi)), bool(_eapi_is_deprecated(eapi)))
            for eapi in sorted(eapis)
        ),
        arch=settings.get("ARCH", ""),
    )


def read(portdb):
    """The whole index."""
    return assemble(portdb, read_versions(portdb, portdb.cp_all()))


def to_json(index):
    """The index as canonical JSON: sorted keys, everything in the store's order."""
    v = index.visibility
    document = {
        "format": 8,
        "binaries": [
            {
                "cpv": b.cpv,
                "slot": b.slot,
                "source": b.source,
                "build_id": b.build_id,
                "size": b.size,
                "use": list(b.use),
                "iuse": list(b.iuse),
                "ignored": b.ignored,
            }
            for b in index.binaries
        ],
        "binary_sources": [
            {"kind": s.kind, "name": s.name, "location": s.location}
            for s in index.binary_sources
        ],
        "advisories": [
            {
                "id": a.id,
                "title": a.title,
                "synopsis": a.synopsis,
                "revision": a.revision,
                "packages": [
                    {
                        "cp": p.cp,
                        "arch": p.arch,
                        "vulnerable": list(p.vulnerable),
                        "unaffected": list(p.unaffected),
                    }
                    for p in a.packages
                ],
            }
            for a in index.advisories
        ],
        "repositories": [
            {
                "name": r.name,
                "location": r.location,
                "description_index": r.description_index,
            }
            for r in index.repositories
        ],
        "versions": [
            {
                "cp": x.cp,
                "cpv": x.cpv,
                "repo": x.repo,
                "slot": x.slot,
                "sub_slot": x.sub_slot,
                "eapi": x.eapi,
                "keywords": list(x.keywords),
                "license": list(x.license),
                "properties": list(x.properties),
                "restrict": list(x.restrict),
                "use": list(x.use),
                "description": x.description,
                "homepage": x.homepage,
                "invalid": list(x.invalid),
                "inherited": list(x.inherited),
                "fetch": [
                    {"places": [list(p) for p in g.places], "bytes": g.bytes}
                    for g in x.fetch
                ],
            }
            for x in index.versions
        ],
        "visibility": {
            "eapis": [
                {"eapi": e.eapi, "supported": e.supported, "deprecated": e.deprecated}
                for e in v.eapis
            ],
            "arch": v.arch,
        },
        "ledger": ledger.visibility_to_json(index.ledger),
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
