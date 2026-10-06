"""Full and incremental builds of the installed layer, with the inputs they read."""

import os
import stat
import time
from typing import NamedTuple

import portage
from portage.dep import Atom
from portage.versions import cpv_getkey

from egraph_build import __version__, evaluated, installed, repository, roots
from egraph_build.installed import ATOM, STRONG_BLOCKER, WEAK_BLOCKER, InstalledLayer
from egraph_build.store import (
    INPUT_DIRECTORY,
    INPUT_FILE,
    INPUT_MISSING,
    INPUT_SYMLINK,
    Input,
)

# Timestamps are coarse, so an input modified this close to the start of a build may have
# changed again within the same tick; such inputs are never trusted as unchanged.
RACY_WINDOW_NS = 1_000_000_000


class Build(NamedTuple):
    layer: InstalledLayer
    inputs: tuple
    # When the build started reading; the store records it for the racy check.
    started_ns: int
    # Packages read from the vdb; the others were carried over from the previous store.
    evaluated: frozenset
    full: bool


def stat_input(path):
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return Input(path, INPUT_MISSING, 0, 0)
    if stat.S_ISLNK(st.st_mode):
        kind = INPUT_SYMLINK
    elif stat.S_ISDIR(st.st_mode):
        kind = INPUT_DIRECTORY
    else:
        kind = INPUT_FILE
    # Pre-1970 timestamps are clamped on both sides; the format stores them unsigned.
    return Input(path, kind, max(st.st_mtime_ns, 0), st.st_size)


def vdb_path(settings):
    return os.path.join(settings["EROOT"], portage.const.VDB_PATH)


def _entries(directory):
    try:
        return sorted(os.path.join(directory, name) for name in os.listdir(directory))
    except (FileNotFoundError, NotADirectoryError):
        return []


def config_paths(settings):
    """Everything portage's config read that can change how installed atoms match.

    USE-dep defaults depend on the profile's implicit IUSE, which make.globals, make.conf and
    the profile stack set.
    """
    config_root = settings["PORTAGE_CONFIGROOT"]
    user = os.path.join(config_root, portage.const.USER_CONFIG_PATH)
    paths = [
        os.path.join(
            settings["EPREFIX"], portage.const.GLOBAL_CONFIG_PATH, "make.globals"
        ),
        os.path.join(config_root, "etc/make.conf"),
        os.path.join(user, "make.conf"),
        os.path.join(user, "make.profile"),
        os.path.join(user, "profile"),
    ]
    for directory in (os.path.join(user, "make.conf"), os.path.join(user, "profile")):
        paths.extend(_entries(directory))
    for profile in settings.profiles:
        paths.append(profile)
        paths.extend(_entries(profile))
    return paths


def builder_paths():
    """The builder's own modules: a changed builder can read any package differently."""
    here = os.path.dirname(os.path.abspath(__file__))
    return [
        os.path.join(here, name) for name in os.listdir(here) if name.endswith(".py")
    ]


# The user's configuration that decides visibility and USE, beyond what config_paths covers.
USER_VISIBILITY_CONFIG = (
    "categories",
    "env",
    "package.accept_keywords",
    "package.accept_restrict",
    "package.env",
    "package.keywords",
    "package.license",
    "package.mask",
    "package.properties",
    "package.unmask",
    "package.use",
    "package.use.force",
    "package.use.mask",
    "package.use.stable.force",
    "package.use.stable.mask",
    "repos.conf",
)


def _tree(path):
    """path, and everything below it when it is a directory."""
    found = [path]
    for directory, dirs, files in os.walk(path):
        dirs.sort()
        found.extend(os.path.join(directory, name) for name in dirs + sorted(files))
    return found


def repository_paths(portdb, cps):
    """What a repository's answers about cps depend on.

    The main repository changes by sync, which replaces files, so its metadata cache
    directories show every change. Other repositories may be edited in place and may have no
    cache, so their package directories and ebuilds count too. Every category directory
    counts, for the cps it lists.
    """
    main = portdb.repositories.mainRepo()
    categories = sorted({cp.partition("/")[0] for cp in cps})
    paths = []
    for name in portdb.getRepositories():
        location = portdb.getRepositoryPath(name)
        paths.append(location)
        paths.extend(
            os.path.join(location, category)
            for category in sorted(portdb.settings.categories)
            if os.path.isdir(os.path.join(location, category))
        )
        for relative in (
            "eclass",
            "metadata/layout.conf",
            "profiles/categories",
            "profiles/license_groups",
        ):
            paths.append(os.path.join(location, relative))
        paths.extend(_tree(os.path.join(location, "profiles", "package.mask")))
        paths.extend(_tree(os.path.join(location, "profiles", "updates")))
        cache = os.path.join(location, "metadata", "md5-cache")
        paths.append(cache)
        paths.extend(
            os.path.join(cache, category)
            for category in categories
            if os.path.isdir(os.path.join(cache, category))
        )
        if main is not None and name == main.name:
            continue
        for cp in cps:
            directory = os.path.join(location, cp)
            if os.path.isdir(directory):
                paths.append(directory)
                paths.extend(
                    path for path in _entries(directory) if path.endswith(".ebuild")
                )
    return paths


def _evaluated_paths(settings, portdb, cps):
    user = os.path.join(settings["PORTAGE_CONFIGROOT"], portage.const.USER_CONFIG_PATH)
    paths = config_paths(settings)
    for name in USER_VISIBILITY_CONFIG:
        paths.extend(_tree(os.path.join(user, name)))
    paths.extend(repository_paths(portdb, cps))
    return paths


def evaluated_inputs(settings, portdb, cps):
    """The inputs of an evaluated layer answering for cps (EvaluatedLayer.cps)."""
    paths = _evaluated_paths(settings, portdb, cps)
    return tuple(sorted({stat_input(path) for path in paths}))


def repository_inputs(portdb):
    """The inputs of the repository index: every cp's, the configuration and the builder."""
    paths = _evaluated_paths(portdb.settings, portdb, portdb.cp_all())
    paths.extend(builder_paths())
    return tuple(sorted({stat_input(path) for path in paths}))


def collect_inputs(settings, cpvs):
    vdb = vdb_path(settings)
    categories = sorted({cpv.partition("/")[0] for cpv in cpvs})
    paths = [vdb]
    paths.extend(os.path.join(vdb, category) for category in categories)
    paths.extend(os.path.join(vdb, cpv) for cpv in cpvs)
    paths.extend(config_paths(settings))
    paths.extend(roots.input_paths(settings))
    paths.extend(builder_paths())
    return tuple(sorted({stat_input(path) for path in paths}))


def _cpvs(vardb):
    return sorted(str(cpv) for cpv in vardb.cpv_all())


def full(vardb):
    started = time.time_ns()
    cpvs = _cpvs(vardb)
    inputs = collect_inputs(vardb.settings, cpvs)
    match = installed.Matcher(vardb)
    layer = InstalledLayer(
        (installed.read_package(vardb, cpv, match) for cpv in cpvs),
        roots.read_roots(vardb, match),
    )
    return Build(layer, inputs, started, frozenset(cpvs), True)


class EvaluatedBuild(NamedTuple):
    layer: evaluated.EvaluatedLayer
    inputs: tuple
    started_ns: int
    # The cps read through portage; the others were carried over.
    evaluated: frozenset
    full: bool


def evaluate(vardb, portdb, requested=()):
    """A full build of the evaluated layer, with the requested cps (EvaluatedLayer.requested)."""
    started = time.time_ns()
    layer, read = evaluated.rebuild(vardb, portdb, None, None, requested=requested)
    # Stat'ed after the build, which the racy window allows: anything changed since started
    # is newer than it.
    inputs = evaluated_inputs(vardb.settings, portdb, layer.cps())
    return EvaluatedBuild(layer, inputs, started, read, True)


class RepositoryBuild(NamedTuple):
    index: repository.RepositoryIndex
    inputs: tuple
    started_ns: int
    # The cps read through portage; the others' versions were carried over.
    reread: frozenset
    full: bool


def index(portdb):
    """A full build of the repository index."""
    started = time.time_ns()
    found = repository.read(portdb)
    cps = frozenset(v.cp for v in found.versions)
    return RepositoryBuild(found, repository_inputs(portdb), started, cps, True)


# An index input that bears only on the visibility configuration.
_CONFIGURATION = "configuration"


def _index_scope(path, locations, categories):
    """The category or cp an index input bears on alone, _CONFIGURATION, or None when it can
    bear on any version. locations are the repositories', longest first."""
    for location in locations:
        if not path.startswith(location + os.sep):
            continue
        parts = path[len(location) + 1 :].split(os.sep)
        if parts[:2] == ["metadata", "md5-cache"]:
            parts = parts[2:]
            return parts[0] if len(parts) == 1 and parts[0] in categories else None
        if parts[0] in categories and len(parts) <= 3:
            return "/".join(parts[:2])
        if parts[0] == "profiles" and parts[1:2] in (
            ["package.mask"],
            ["license_groups"],
            ["updates"],
        ):
            return _CONFIGURATION
        return None
    return None


def index_incremental(portdb, previous):
    """Rebuild the repository index from a previous one, reading again only the cps whose
    metadata changed; after a configuration change, only the visibility configuration and the
    USE of the versions that keep one.

    previous is that index's (RepositoryMeta, Inputs, RepositoryIndex). Another EROOT, egraph
    or portage version, other repositories, a changed builder, or any change an input cannot
    pin to a category or cp (an overlay's eclasses, the categories, repos.conf) mean a full
    build.
    """
    settings = portdb.settings
    meta, inputs, old = previous
    built_by = (meta.egraph_version, meta.portage_version)
    if (
        meta.eroot != settings["EROOT"]
        or built_by != (__version__, portage.VERSION)
        or old.repositories != repository.repositories(portdb)
    ):
        return index(portdb)
    started = time.time_ns()
    current = repository_inputs(portdb)
    racy_after = meta.build_time_ns - RACY_WINDOW_NS
    recorded = {item.path: item for item in inputs}
    by_path = {item.path: item for item in current}
    kind_only = _kind_only(portdb)
    builder = set(builder_paths())
    user = os.path.join(settings["PORTAGE_CONFIGROOT"], portage.const.USER_CONFIG_PATH)
    configuration = set(config_paths(settings))
    for name in USER_VISIBILITY_CONFIG:
        if name not in ("categories", "repos.conf"):
            configuration.update(_tree(os.path.join(user, name)))
    categories = frozenset(settings.categories)
    locations = sorted(
        (location for _, location in old.repositories), key=len, reverse=True
    )
    scopes = set()
    reconfigured = False
    for path in recorded.keys() | by_path.keys():
        before, after = recorded.get(path), by_path.get(path)
        if before is not None and after is not None:
            if before == after and before.mtime_ns < racy_after:
                continue
            if path in kind_only and before.kind == after.kind:
                continue
        if path in builder:
            return index(portdb)
        scope = (
            _CONFIGURATION
            if path in configuration
            else _index_scope(path, locations, categories)
        )
        if scope is None:
            return index(portdb)
        if scope == _CONFIGURATION:
            reconfigured = True
        else:
            scopes.add(scope)
    cps = set(portdb.cp_all())
    old_cps = {v.cp for v in old.versions}
    dirty = frozenset(
        cp for cp in cps | old_cps if cp in scopes or cp.partition("/")[0] in scopes
    )
    settings_holder = repository.UseReader(portdb)
    read = {}
    for version in repository.read_versions(portdb, dirty & cps, settings_holder):
        read.setdefault(version.cp, []).append(version)
    kept = {}
    for version in old.versions:
        if version.cp not in dirty:
            if reconfigured:
                version = repository.with_use(version, settings_holder)
            kept.setdefault(version.cp, []).append(version)
    versions = [v for cp in sorted(cps) for v in kept.get(cp) or read.get(cp, ())]
    return RepositoryBuild(
        repository.assemble(portdb, versions), current, started, dirty, False
    )


def _kind_only(portdb):
    """Evaluated inputs whose kind alone counts: a repository's root and its metadata cache
    only list what is an input of its own, and the main repository's metadata cache records
    each ebuild's eclasses."""
    main = portdb.repositories.mainRepo()
    found = set()
    for name in portdb.getRepositories():
        location = portdb.getRepositoryPath(name)
        cache = os.path.join(location, "metadata", "md5-cache")
        found.update((location, cache))
        if main is not None and name == main.name and os.path.isdir(cache):
            found.add(os.path.join(location, "eclass"))
    return found


def _scope(path, locations, categories, known_categories):
    """The installed category or cp an evaluated input bears on alone, "" when it only lists
    cps, or None when it can bear on any package. locations are the repositories', longest
    first."""
    for location in locations:
        if not path.startswith(location + os.sep):
            continue
        parts = path[len(location) + 1 :].split(os.sep)
        if parts[:2] == ["metadata", "md5-cache"]:
            parts = parts[2:]
            if len(parts) != 1:
                return None
        elif len(parts) == 1 and parts[0] in known_categories - categories:
            return ""
        if parts[0] not in categories or len(parts) > 3:
            return None
        return "/".join(parts[:2])
    return None


def evaluate_incremental(
    vardb, portdb, previous, installed_build, installed_build_ns, requested=()
):
    """Rebuild the evaluated layer from a previous evaluated store, evaluating only the cps
    whose installed packages or repository metadata changed, and the requested cps new to it.

    previous is that store's (EvaluatedMeta, Inputs, EvaluatedLayer), installed_build the Build
    of the installed store written beside the new one, and installed_build_ns the build start
    of the installed store it replaces. The vdb changes come from installed_build, so a full
    installed build, or a previous store built against another one, means a full build here.
    Either way the previous store's requested cps are kept.
    """
    settings = vardb.settings
    meta, inputs, layer = previous
    requested = frozenset(layer.requested()).union(requested)
    if (
        installed_build.full
        or meta.eroot != settings["EROOT"]
        or meta.installed_build_time_ns != installed_build_ns
    ):
        return evaluate(vardb, portdb, requested)
    started = time.time_ns()
    cpvs = _cpvs(vardb)
    cps = sorted(set(layer.cps()) | {cpv_getkey(cpv) for cpv in cpvs})
    current = evaluated_inputs(settings, portdb, cps)
    racy_after = meta.build_time_ns - RACY_WINDOW_NS
    recorded = {item.path: item for item in inputs}
    by_path = {item.path: item for item in current}
    kind_only = _kind_only(portdb)
    previous_cpvs = set(layer.installed())
    categories = {cp.partition("/")[0] for cp in cps}
    known_categories = frozenset(portdb.settings.categories)
    locations = sorted(
        (portdb.getRepositoryPath(name) for name in portdb.getRepositories()),
        key=len,
        reverse=True,
    )
    scopes = set()
    for path in recorded.keys() | by_path.keys():
        old, new = recorded.get(path), by_path.get(path)
        if old is not None and new is not None:
            if old == new and old.mtime_ns < racy_after:
                continue
            if path in kind_only and old.kind == new.kind:
                continue
        scope = _scope(path, locations, categories, known_categories)
        if scope is None:
            return evaluate(vardb, portdb, requested)
        if scope:
            scopes.add(scope)
    removed = previous_cpvs - set(cpvs)
    touched = {cpv_getkey(cpv) for cpv in installed_build.evaluated | removed}
    dirty = frozenset(
        cp
        for cp in touched.union(cps)
        if cp in touched or cp in scopes or cp.partition("/")[0] in scopes
    )
    match = installed.Matcher(vardb)
    ev, read = evaluated.rebuild(
        vardb,
        portdb,
        layer,
        dirty,
        _Rematcher(touched, match),
        match,
        requested,
    )
    if ev.cps() != cps:
        current = evaluated_inputs(settings, portdb, ev.cps())
    return EvaluatedBuild(ev, current, started, read, False)


class _Rematcher:
    """Re-resolves the atoms of an unchanged package that name a touched cp."""

    def __init__(self, touched, match):
        self._touched = touched
        self._match = match
        self._atoms = {}

    def _atom(self, text):
        atom = self._atoms.get(text)
        if atom is None:
            atom = self._atoms[text] = Atom(text.lstrip("!"))
        return atom

    def _rematched(self, item):
        """item, an atom node or a Possible, with its matches found again if it names a
        touched cp; None when it does not."""
        atom = self._atom(item.atom)
        if atom.cp not in self._touched:
            return None
        return item._replace(matches=self._match(atom))

    def _trees(self, trees):
        """(trees, whether any node was rematched)"""
        found = []
        changed = False
        for nodes in trees:
            out = []
            for node in nodes:
                if node.type in (ATOM, WEAK_BLOCKER, STRONG_BLOCKER):
                    rematched = self._rematched(node)
                    if rematched is not None:
                        node = rematched
                        changed = True
                out.append(node)
            found.append(tuple(out))
        return tuple(found), changed

    def __call__(self, pkg):
        """An installed.Package, or an evaluated.Dependencies with its possible entries."""
        if not self._touched:
            return pkg
        deps, changed = self._trees(pkg.deps)
        if changed:
            pkg = pkg._replace(deps=deps)
        if isinstance(pkg, evaluated.Dependencies) and pkg.possible:
            possible = tuple(self._rematched(p) or p for p in pkg.possible)
            pkg = pkg._replace(possible=possible)
        return pkg


def incremental(vardb, meta, inputs, layer):
    """Rebuild from a previous store, reading only what changed since it was built.

    A changed config input or builder module, or a store built for another EROOT or by another
    egraph or portage version, means a full build: the profile decides how every USE
    dependency matches, and the builder and portage how every package reads. Roots are cheap and always read again,
    so a world or set file edit is not a config change.
    """
    settings = vardb.settings
    built_by = (meta.egraph_version, meta.portage_version)
    if meta.eroot != settings["EROOT"] or built_by != (__version__, portage.VERSION):
        return full(vardb)
    started = time.time_ns()
    cpvs = _cpvs(vardb)
    current = collect_inputs(settings, cpvs)
    recorded = {item.path: item for item in inputs}
    racy_after = meta.build_time_ns - RACY_WINDOW_NS

    def changed(item):
        old = recorded.get(item.path)
        return old is None or old != item or old.mtime_ns >= racy_after

    vdb = vdb_path(settings)
    root_inputs = set(roots.input_paths(settings))

    def is_config(path):
        in_vdb = path == vdb or path.startswith(vdb + os.sep)
        return not in_vdb and path not in root_inputs

    current_paths = {item.path for item in current}
    if any(changed(item) for item in current if is_config(item.path)) or any(
        path not in current_paths for path in recorded if is_config(path)
    ):
        return full(vardb)

    by_path = {item.path: item for item in current}
    previous = set(layer.installed())
    reread = {
        cpv
        for cpv in cpvs
        if cpv not in previous or changed(by_path[os.path.join(vdb, cpv)])
    }
    touched = {cpv_getkey(cpv) for cpv in reread | (previous - set(cpvs))}
    match = installed.Matcher(vardb)
    rematch = _Rematcher(touched, match)
    packages = [
        (
            installed.read_package(vardb, cpv, match)
            if cpv in reread
            else rematch(layer.package(cpv))
        )
        for cpv in cpvs
    ]
    known = {root.atom: root.matches for root in layer.roots()}

    def match_root(atom):
        found = known.get(str(atom))
        return match(atom) if found is None or atom.cp in touched else found

    layer = InstalledLayer(packages, roots.read_roots(vardb, match_root))
    return Build(layer, current, started, frozenset(reread), False)
