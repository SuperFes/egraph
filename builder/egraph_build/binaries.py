"""The binary packages emerge could use instead of building: those portage's indexes list, the
local PKGDIR's and the copies it keeps of its binhosts' (docs/store-format.md). Nothing is
fetched: a binhost portage never fetched from lists nothing.
"""

import os
from typing import NamedTuple

# Source kinds.
LOCAL, BINHOST = 0, 1

# Ignored bits: what emerge would refuse a binary for apart from its USE.
CHANGED_DEPS = 1
USER_PATCHES = 2

# What --binpkg-changed-deps compares under --usepkg, which leaves out build-time deps.
_RUNTIME_KEYS = ("IDEPEND", "PDEPEND", "RDEPEND")


class Source(NamedTuple):
    kind: int
    # The binhost's name in binrepos.conf, empty for PKGDIR.
    name: str
    # PKGDIR, or the binhost's URI.
    location: str


class Binary(NamedTuple):
    cpv: str
    slot: str
    # Into the index's binary sources.
    source: int
    build_id: int
    size: int
    use: tuple
    iuse: tuple
    ignored: int = 0


def _package_index(settings):
    """An empty PackageIndex as bintree reads its own; bintree's keys are private to it."""
    from portage.dbapi.bintree import binarytree

    tree = binarytree(pkgdir=settings["PKGDIR"], settings=settings)
    return tree._new_pkgindex()


def binhosts(settings):
    """The binhosts binrepos.conf and PORTAGE_BINHOST name, by descending priority."""
    from portage.binrepo.config import BinRepoConfigLoader
    from portage.const import BINREPOS_CONF_FILE

    paths = [
        os.path.join(settings.global_config_path, "binrepos.conf"),
        os.path.join(settings["PORTAGE_CONFIGROOT"], BINREPOS_CONF_FILE),
    ]
    return list(reversed(list(BinRepoConfigLoader(paths, settings).values())))


def config_paths(settings):
    """The binrepos.conf files binhosts reads."""
    from portage.const import BINREPOS_CONF_FILE

    user = os.path.join(settings["PORTAGE_CONFIGROOT"], BINREPOS_CONF_FILE)
    paths = [os.path.join(settings.global_config_path, "binrepos.conf"), user]
    if os.path.isdir(user):
        paths.extend(
            os.path.join(directory, name)
            for directory, _, files in os.walk(user)
            for name in sorted(files)
        )
    return paths


def cache_path(settings, uri):
    """Where portage keeps its copy of the index of the binhost at uri."""
    from urllib.parse import urlparse

    from portage.const import CACHE_PATH

    parsed = urlparse(uri)
    return os.path.join(
        settings["EROOT"],
        CACHE_PATH,
        "binhost",
        parsed.hostname or "",
        parsed.path.lstrip("/"),
        "Packages",
    )


def index_paths(settings):
    """Each index read, PKGDIR's first."""
    return [os.path.join(settings["PKGDIR"], "Packages")] + [
        cache_path(settings, repo.sync_uri) for repo in binhosts(settings)
    ]


def _read_index(settings, path):
    index = _package_index(settings)
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            index.read(f)
    except FileNotFoundError:
        return None
    return index


def _int(text):
    try:
        return int(text)
    except (TypeError, ValueError):
        return 0


def _iuse(text):
    return tuple(sorted({flag.lstrip("+-") for flag in text.split()}))


def _admitted(settings, repo):
    """Whether emerge would read a binhost's entry: its include and exclude lists, and the gpkg
    format where signatures are required."""
    from portage.exception import InvalidBinaryPackageFormat
    from portage.binpkg import get_binpkg_format

    gpkg_only = "binpkg-request-signature" in settings.features or repo.verify_signature
    exclude = repo.getbinpkg_exclude
    include = repo.getbinpkg_include

    def admitted(entry, cpv):
        if exclude is not None and not exclude.isEmpty() and exclude.containsCPV(cpv):
            return False
        if include is not None and not include.isEmpty():
            if not include.containsCPV(cpv):
                return False
        if gpkg_only:
            try:
                return get_binpkg_format(entry.get("PATH"), remote=True) == "gpkg"
            except InvalidBinaryPackageFormat:
                return False
        return True

    return admitted


def _strip_slots(tree):
    """As portage's strip_slots, which imports _emerge: a slot operator's slot and sub-slot
    left out, its "=" kept."""
    for i, x in enumerate(tree):
        if isinstance(x, list):
            _strip_slots(x)
        elif getattr(x, "slot_operator", None) == "=" and x.slot is not None:
            tree[i] = x.with_slot("=")


class Ignored:
    """What emerge would refuse each binary for apart from its USE (Binary.ignored)."""

    def __init__(self, portdb, vardb):
        self._portdb = portdb
        self._vardb = vardb
        self._libc = None

    def _libc_cps(self):
        from portage.dbapi.vartree import vartree
        from portage.dep.libc import find_libc_deps

        if self._libc is None:
            if self._vardb is None:
                self._vardb = vartree(settings=self._portdb.settings).dbapi
            self._libc = {atom.cp for atom in find_libc_deps(self._vardb, False)}
        return self._libc

    def _reduced(self, text, use, eapi):
        from portage.dep import Atom, use_reduce

        tree = use_reduce(text, uselist=use, eapi=eapi, token_class=Atom)
        _strip_slots(tree)
        libc = self._libc_cps()
        return [x for x in tree if not (isinstance(x, Atom) and x.cp in libc)]

    def changed_deps(self, cpv, repo, entry, use):
        """As depgraph's _changed_deps: the binary's runtime deps under its USE against those of
        the ebuild of its cpv (its repository's, else the first) under the same."""
        from portage.exception import InvalidDependString

        keys = ("EAPI",) + _RUNTIME_KEYS
        metadata = None
        repos = ([repo] if repo in self._portdb.getRepositories() else []) + [None]
        for name in repos:
            try:
                metadata = dict(
                    zip(keys, self._portdb.aux_get(cpv, list(keys), myrepo=name))
                )
                break
            except KeyError:
                continue
        if metadata is None:
            return False
        try:
            built = [
                self._reduced(entry.get(k, ""), use, entry.get("EAPI", "0"))
                for k in _RUNTIME_KEYS
            ]
        except InvalidDependString:
            return True
        unbuilt = [
            self._reduced(metadata[k], use, metadata["EAPI"]) for k in _RUNTIME_KEYS
        ]
        return built != unbuilt

    def __call__(self, cpv, entry, use):
        from portage.exception import InvalidDependString

        ignored = 0
        try:
            if self.changed_deps(cpv, entry.get("repository", ""), entry, use):
                ignored |= CHANGED_DEPS
        except InvalidDependString:
            # An ebuild whose deps do not parse; depgraph cannot build it either.
            pass
        settings = self._portdb.settings
        if entry.get("USER_PATCHES", "").lower() != settings.userPatchDigest(cpv):
            ignored |= USER_PATCHES
        return ignored


def _entries(settings, ignored, source, index, admitted, keep):
    from portage.versions import _pkg_str

    found = []
    for entry in index.packages:
        try:
            cpv = _pkg_str(
                entry["CPV"],
                metadata={"SLOT": entry.get("SLOT", "0")},
                settings=settings,
            )
        except Exception:
            continue
        if admitted is not None and not admitted(entry, cpv):
            continue
        use = frozenset(entry.get("USE", "").split())
        build_id = _int(entry.get("BUILD_ID"))
        kept = keep(cpv, source, build_id)
        if kept is not None:
            found.append(kept)
            continue
        found.append(
            Binary(
                cpv=str(cpv),
                slot=entry.get("SLOT", "0").partition("/")[0],
                source=source,
                build_id=build_id,
                size=_int(entry.get("SIZE")),
                use=tuple(sorted(use)),
                iuse=_iuse(entry.get("IUSE", "")),
                ignored=ignored(cpv, entry, use),
            )
        )
    return found


def read(portdb, vardb=None, previous=None, cps=frozenset(), sources=frozenset()):
    """(sources, binaries): every binary the indexes list, in the index's order. Given previous
    binaries of the same sources, those of neither cps nor sources keep their records there.
    """
    settings = portdb.settings
    ignored = Ignored(portdb, vardb)
    records = {(b.cpv, b.source, b.build_id): b for b in previous or ()}

    def keep(cpv, source, build_id):
        if previous is None or cpv.cp in cps or source in sources:
            return None
        return records.get((str(cpv), source, build_id))

    found_sources = [Source(LOCAL, "", settings["PKGDIR"])]
    found = []
    local = _read_index(settings, os.path.join(settings["PKGDIR"], "Packages"))
    if local is not None:
        found.extend(_entries(settings, ignored, 0, local, None, keep))
    for repo in binhosts(settings):
        source = len(found_sources)
        found_sources.append(
            Source(BINHOST, repo.name or repo.name_fallback, repo.sync_uri)
        )
        index = _read_index(settings, cache_path(settings, repo.sync_uri))
        if index is not None:
            admitted = _admitted(settings, repo)
            found.extend(_entries(settings, ignored, source, index, admitted, keep))
    found.sort(key=lambda b: (b.cpv, b.source, b.build_id))
    return tuple(found_sources), tuple(found)


def _patches(settings):
    from portage.const import USER_CONFIG_PATH

    return os.path.join(settings["PORTAGE_CONFIGROOT"], USER_CONFIG_PATH, "patches")


def _virtuals(settings):
    from portage.const import VDB_PATH

    return os.path.join(settings["EROOT"], VDB_PATH, "virtual")


def input_paths(settings):
    """What the binaries depend on beyond the ebuilds: each index, binrepos.conf, the user
    patches, and the installed virtuals, which name the libc."""
    paths = index_paths(settings) + config_paths(settings) + [_virtuals(settings)]
    patches = _patches(settings)
    paths.append(patches)
    for directory, dirs, files in os.walk(patches):
        dirs.sort()
        paths.extend(os.path.join(directory, name) for name in dirs + sorted(files))
    return paths


# An input bearing on every binary.
ALL = "all"


def input_scope(settings, path):
    """What path, an input now or before, bears on of the binaries alone: the index of the
    source whose index it is, ALL, or None when it bears on more than the binaries."""
    from portage.const import BINREPOS_CONF_FILE, CACHE_PATH

    indexes = index_paths(settings)
    if path in indexes:
        return indexes.index(path)
    if path in (
        os.path.join(settings.global_config_path, "binrepos.conf"),
        _virtuals(settings),
    ):
        return ALL
    for parent in (
        os.path.join(settings["EROOT"], CACHE_PATH, "binhost"),
        os.path.join(settings["PORTAGE_CONFIGROOT"], BINREPOS_CONF_FILE),
        _patches(settings),
    ):
        if path == parent or path.startswith(parent + os.sep):
            return ALL
    return None
