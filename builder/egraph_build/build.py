"""Full and incremental builds of the installed layer, with the inputs they read."""

import os
import stat
import time
from typing import NamedTuple

import portage
from portage.dep import Atom
from portage.versions import cpv_getkey

from egraph_build import installed
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


def collect_inputs(settings, cpvs):
    vdb = vdb_path(settings)
    categories = sorted({cpv.partition("/")[0] for cpv in cpvs})
    paths = [vdb]
    paths.extend(os.path.join(vdb, category) for category in categories)
    paths.extend(os.path.join(vdb, cpv) for cpv in cpvs)
    paths.extend(config_paths(settings))
    return tuple(sorted({stat_input(path) for path in paths}))


def _cpvs(vardb):
    return sorted(str(cpv) for cpv in vardb.cpv_all())


def full(vardb):
    started = time.time_ns()
    cpvs = _cpvs(vardb)
    inputs = collect_inputs(vardb.settings, cpvs)
    match = installed.Matcher(vardb)
    layer = InstalledLayer(installed.read_package(vardb, cpv, match) for cpv in cpvs)
    return Build(layer, inputs, started, frozenset(cpvs), True)


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
    profile decides how every USE dependency matches.
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

    def is_config(path):
        return path != vdb and not path.startswith(vdb + os.sep)

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
    return Build(InstalledLayer(packages), current, started, frozenset(reread), False)
