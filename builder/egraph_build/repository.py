"""The repository index: every version in the repositories, and what portage decides their
visibility from, its metadata and its configuration as portage parsed them
(docs/store-format.md). egraph evaluates the visibility itself.

That configuration is private to portage's config; this module is the one place egraph reads it.
"""

import json
from typing import NamedTuple

from egraph_build import evaluated

# What a version record holds of the ebuild's metadata.
KEYS = (
    "DESCRIPTION",
    "EAPI",
    "HOMEPAGE",
    "KEYWORDS",
    "LICENSE",
    "PROPERTIES",
    "RESTRICT",
    "SLOT",
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


class RepositoryIndex(NamedTuple):
    # (name, location), in portage's order.
    repositories: tuple
    versions: tuple
    visibility: Visibility


def _tokens(text):
    return tuple(text.split())


def _tested(tokens):
    return {token.lstrip("!")[:-1] for token in tokens if token.endswith("?")}


def read_versions(portdb, cps):
    """The version records of cps, in the index's order; settings is cloned for the USE of the
    few whose LICENSE or PROPERTIES has a conditional."""
    import portage

    repositories = portdb.getRepositories()
    settings = None
    found = []
    for cp in sorted(cps):
        for repo in repositories:
            for cpv in portdb.cp_list(cp, mytree=portdb.getRepositoryPath(repo)):
                try:
                    metadata = dict(
                        zip(KEYS, portdb.aux_get(cpv, list(KEYS), myrepo=repo))
                    )
                except KeyError:
                    continue
                license_tokens = _tokens(metadata["LICENSE"])
                properties = _tokens(metadata["PROPERTIES"])
                restrict = _tokens(metadata["RESTRICT"])
                use = ()
                if _tested(license_tokens + properties):
                    tested = _tested(license_tokens + properties + restrict)
                    if settings is None:
                        settings = portage.config(clone=portdb.settings)
                    use = _use(portdb, settings, cpv, repo) & tested
                slot, _, sub_slot = metadata["SLOT"].partition("/")
                found.append(
                    Version(
                        cp=cp,
                        cpv=str(cpv),
                        repo=repo,
                        slot=slot,
                        sub_slot=sub_slot or slot,
                        eapi=metadata["EAPI"],
                        keywords=_tokens(metadata["KEYWORDS"]),
                        license=license_tokens,
                        properties=properties,
                        restrict=restrict,
                        use=tuple(sorted(use)),
                        description=metadata["DESCRIPTION"],
                        homepage=metadata["HOMEPAGE"],
                    )
                )
    return tuple(found)


def _use(portdb, settings, cpv, repo):
    """The USE the ebuild would be built with now, as portdbapi's _visible takes it."""
    keys = evaluated._CANDIDATE_KEYS
    try:
        metadata = dict(zip(keys, portdb.aux_get(cpv, list(keys), myrepo=repo)))
    except KeyError:
        return set()
    settings.setcpv(cpv, mydb=metadata)
    return set(settings["PORTAGE_USE"].split())


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
    versions = read_versions(portdb, portdb.cp_all())
    return RepositoryIndex(
        repositories=tuple(
            (name, portdb.getRepositoryPath(name)) for name in portdb.getRepositories()
        ),
        versions=versions,
        visibility=read_visibility(portdb.settings, {v.eapi for v in versions}),
    )


def _entries_json(entries):
    return [{"atom": e.atom, "tokens": list(e.tokens)} for e in entries]


def to_json(index):
    """The index as canonical JSON: sorted keys, everything in the store's order."""
    v = index.visibility
    document = {
        "format": 2,
        "repositories": [
            {"name": name, "location": location}
            for name, location in index.repositories
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
