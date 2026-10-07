"""The repository index: every version in the repositories, and what portage decides their
visibility from, its metadata and its configuration as portage parsed them
(docs/store-format.md), and the main repository's security advisories. egraph evaluates the
visibility, and which advisories affect the system, itself.

That configuration is private to portage's config; this module is the one place egraph reads it.
"""

import json
import os
from typing import NamedTuple

from egraph_build import evaluated, masks

# What a version record holds of the ebuild's metadata, and what its validity is checked on.
KEYS = tuple(
    sorted(
        {
            *masks.EBUILD_KEYS,
            "DESCRIPTION",
            "EAPI",
            "HOMEPAGE",
            "KEYWORDS",
            "LICENSE",
            "PROPERTIES",
            "RESTRICT",
            "SLOT",
        }
    )
)


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


class Entry(NamedTuple):
    """A package.* line: an atom, wildcards allowed, and its tokens."""

    atom: str
    tokens: tuple


class Eapi(NamedTuple):
    eapi: str
    supported: bool
    deprecated: bool


class Visibility(NamedTuple):
    eapis: tuple
    accept_keywords: tuple
    # ACCEPT_KEYWORDS in the environment, which keyword checks stack last.
    environment_keywords: tuple
    arch: str
    # Layers of entries, in the profiles' stacking order.
    profile_keywords: tuple
    profile_accept_keywords: tuple
    accept_keywords_entries: tuple
    masks: tuple
    unmasks: tuple
    accept_license: tuple
    licenses: tuple
    accept_properties: tuple
    properties: tuple
    accept_restrict: tuple
    restrict: tuple


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


def _tokens(text):
    return tuple(text.split())


def _tested(tokens):
    return {token.lstrip("!")[:-1] for token in tokens if token.endswith("?")}


class UseReader:
    """A config clone made on first use, for the USE of the few versions that need it."""

    def __init__(self, portdb):
        self._portdb = portdb
        self._settings = None

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
        self._settings.setcpv(cpv, mydb=metadata)
        return set(self._settings["PORTAGE_USE"].split())


def with_use(version, settings):
    """version with the USE its LICENSE, PROPERTIES and RESTRICT conditionals test, where
    LICENSE or PROPERTIES has one."""
    if not _tested(version.license + version.properties):
        return version._replace(use=())
    tested = _tested(version.license + version.properties + version.restrict)
    use = settings.use(version.cpv, version.repo) & tested
    return version._replace(use=tuple(sorted(use)))


def read_versions(portdb, cps, settings=None):
    """The version records of cps in every repository, in the index's order."""
    settings = settings or UseReader(portdb)
    found = []
    for cp in sorted(cps):
        for repo in portdb.getRepositories():
            for cpv in portdb.cp_list(cp, mytree=portdb.getRepositoryPath(repo)):
                try:
                    metadata = dict(
                        zip(KEYS, portdb.aux_get(cpv, list(KEYS), myrepo=repo))
                    )
                except KeyError:
                    continue
                metadata["repository"] = repo
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


def assemble(portdb, versions, advisories=None):
    """The index of versions, with the repositories, the visibility configuration and, unless
    given, the advisories now."""
    return RepositoryIndex(
        repositories=repositories(portdb),
        versions=tuple(versions),
        visibility=read_visibility(portdb.settings, {v.eapi for v in versions}),
        advisories=(
            read_advisories(portdb.settings) if advisories is None else advisories
        ),
    )


def _net(tokens):
    """Incremental tokens (x, -x, *, -*) as their net effect: the last * or -* if any, then the
    last word on each name after it, sorted. A license group's expansion comes from a set, so
    its order would change from one run to the next."""
    reset = None
    last = {}
    for token in tokens:
        if token in ("*", "-*"):
            reset = token
            last.clear()
        else:
            last[token.lstrip("-")] = token
    return ((reset,) if reset else ()) + tuple(sorted(last.values()))


def _layer(cpdict):
    """The entries of a plain {cp: {atom: tokens}} dict, in its order."""
    return tuple(
        Entry(str(atom), tuple(tokens))
        for atoms in cpdict.values()
        for atom, tokens in atoms.items()
    )


def _extended(atom_dict, tokens_of=tuple):
    """The entries of an ExtendedAtomDict of {atom: tokens}, plain cps first."""
    return tuple(
        Entry(str(atom), tokens_of(tokens))
        for _, atoms in atom_dict.iteritems()
        for atom, tokens in atoms.items()
    )


def _atoms(atom_dict):
    """The atoms of an ExtendedAtomDict of lists, plain cps first."""
    return tuple(str(atom) for _, atoms in atom_dict.iteritems() for atom in atoms)


def read_visibility(settings, eapis):
    from portage import _eapi_is_deprecated, eapi_is_supported

    keywords = settings._keywords_manager
    masks = settings._mask_manager
    licenses = settings._license_manager
    return Visibility(
        eapis=tuple(
            Eapi(eapi, bool(eapi_is_supported(eapi)), bool(_eapi_is_deprecated(eapi)))
            for eapi in sorted(eapis)
        ),
        accept_keywords=_tokens(settings.get("ACCEPT_KEYWORDS", "")),
        environment_keywords=_tokens(
            settings.configdict["backupenv"].get("ACCEPT_KEYWORDS", "")
        ),
        arch=settings.get("ARCH", ""),
        profile_keywords=tuple(_layer(layer) for layer in keywords._pkeywords_list),
        profile_accept_keywords=tuple(
            _layer(layer) for layer in keywords._p_accept_keywords
        ),
        accept_keywords_entries=_extended(keywords.pkeywordsdict),
        masks=_atoms(masks._pmaskdict),
        unmasks=_atoms(masks._punmaskdict),
        accept_license=_net(licenses._accept_license),
        licenses=_extended(licenses._plicensedict, _net),
        accept_properties=tuple(settings._accept_properties),
        properties=_extended(settings._ppropertiesdict),
        accept_restrict=tuple(settings._accept_restrict),
        restrict=_extended(settings._paccept_restrict),
    )


def read(portdb):
    """The whole index."""
    return assemble(portdb, read_versions(portdb, portdb.cp_all()))


def _entries_json(entries):
    return [{"atom": e.atom, "tokens": list(e.tokens)} for e in entries]


def to_json(index):
    """The index as canonical JSON: sorted keys, everything in the store's order."""
    v = index.visibility
    document = {
        "format": 4,
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
            }
            for x in index.versions
        ],
        "visibility": {
            "eapis": [
                {"eapi": e.eapi, "supported": e.supported, "deprecated": e.deprecated}
                for e in v.eapis
            ],
            "accept_keywords": list(v.accept_keywords),
            "environment_keywords": list(v.environment_keywords),
            "arch": v.arch,
            "profile_keywords": [_entries_json(layer) for layer in v.profile_keywords],
            "profile_accept_keywords": [
                _entries_json(layer) for layer in v.profile_accept_keywords
            ],
            "accept_keywords_entries": _entries_json(v.accept_keywords_entries),
            "masks": list(v.masks),
            "unmasks": list(v.unmasks),
            "accept_license": list(v.accept_license),
            "licenses": _entries_json(v.licenses),
            "accept_properties": list(v.accept_properties),
            "properties": _entries_json(v.properties),
            "accept_restrict": list(v.accept_restrict),
            "restrict": _entries_json(v.restrict),
        },
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"
