"""Full and incremental builds of the installed layer, with the inputs they read."""

import os
import stat
import time
from typing import NamedTuple

import portage
from portage.dep import Atom
from portage.versions import cpv_getkey

from egraph_build import evaluated, installed, roots
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
    cache, so their package directories and ebuilds count too.
    """
    main = portdb.repositories.mainRepo()
    categories = sorted({cp.partition("/")[0] for cp in cps})
    paths = []
    for name in portdb.getRepositories():
        location = portdb.getRepositoryPath(name)
        paths.append(location)
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
        paths.extend(
            os.path.join(location, category)
            for category in categories
            if os.path.isdir(os.path.join(location, category))
        )
        for cp in cps:
            directory = os.path.join(location, cp)
            if os.path.isdir(directory):
                paths.append(directory)
                paths.extend(
                    path for path in _entries(directory) if path.endswith(".ebuild")
                )
    return paths


def evaluated_inputs(settings, portdb, cpvs):
    user = os.path.join(settings["PORTAGE_CONFIGROOT"], portage.const.USER_CONFIG_PATH)
    paths = config_paths(settings)
    for name in USER_VISIBILITY_CONFIG:
        paths.extend(_tree(os.path.join(user, name)))
    paths.extend(repository_paths(portdb, sorted({cpv_getkey(cpv) for cpv in cpvs})))
    return tuple(sorted({stat_input(path) for path in paths}))


def collect_inputs(settings, cpvs):
    vdb = vdb_path(settings)
    categories = sorted({cpv.partition("/")[0] for cpv in cpvs})
    paths = [vdb]
    paths.extend(os.path.join(vdb, category) for category in categories)
    paths.extend(os.path.join(vdb, cpv) for cpv in cpvs)
    paths.extend(config_paths(settings))
    paths.extend(roots.input_paths(settings))
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


def evaluate(vardb, portdb):
    """A full build of the evaluated layer."""
    started = time.time_ns()
    inputs = evaluated_inputs(vardb.settings, portdb, _cpvs(vardb))
    return EvaluatedBuild(evaluated.build(vardb, portdb), inputs, started)


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

    def __call__(self, pkg):
        deps = []
        changed = False
        for nodes in pkg.deps:
            out = []
            for node in nodes:
                if node.type in (ATOM, WEAK_BLOCKER, STRONG_BLOCKER):
                    atom = self._atom(node.atom)
                    if atom.cp in self._touched:
                        node = node._replace(matches=self._match(atom))
                        changed = True
                out.append(node)
            deps.append(tuple(out))
        return pkg._replace(deps=tuple(deps)) if changed else pkg


def incremental(vardb, meta, inputs, layer):
    """Rebuild from a previous store, reading only what changed since it was built.

    A changed config input, or a store built for another EROOT, means a full build: the
    profile decides how every USE dependency matches. Roots are cheap and always read again,
    so a world or set file edit is not a config change.
    """
    settings = vardb.settings
    if meta.eroot != settings["EROOT"]:
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
