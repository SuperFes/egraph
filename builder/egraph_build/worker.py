"""Builds and merges packages, and uninstalls them, one at a time as emerge does, for egraph-exec.

Requests come one per line on stdin, as JSON objects:
- {"cpv": ..., "repo": ...} builds and merges the ebuild, with "blockers": [cpv, ...] the
  installed packages it blocks or that block it, which the merge may take files over from and an
  uninstall removes after it, and "world": atom the atom to record in the world file once merged;
- {"uninstall": cpv} uninstalls the installed package, with "clean_world": true dropping the
  world file's atoms that then match nothing installed.
For each, events go to stdout one per line: {"phase": name} as each phase starts (a merge's
build phases, then "merge"; an uninstall's "unmerge"), then {"merged": cpv} or
{"uninstalled": cpv}, or {"failed": phase, "status": n, "log": path} when a phase fails, or
{"error": message} for a request that cannot be tried. Whatever portage or an ebuild prints goes
to stderr or the build log, never among the events.
"""

import json
import os
import sys
from typing import NamedTuple, Optional

# emerge's EbuildExecuter's, after the clean before a build. pkg_pretend runs with setup, and
# the fetch with unpack, as doebuild runs what a phase needs first.
PHASES = (
    "clean",
    "setup",
    "unpack",
    "prepare",
    "configure",
    "compile",
    "test",
    "install",
)


class Merge(NamedTuple):
    cpv: str
    repo: str
    blockers: tuple = ()
    world: Optional[str] = None


class Uninstall(NamedTuple):
    cpv: str
    clean_world: bool = False


def parse_request(line):
    """A Merge or Uninstall from its line; ValueError for anything else."""
    from portage.dep import Atom
    from portage.exception import InvalidAtom
    from portage.versions import catpkgsplit

    try:
        fields = json.loads(line)
    except json.JSONDecodeError as e:
        raise ValueError(f"not JSON: {e}") from None
    if not isinstance(fields, dict):
        raise ValueError("not a JSON object")

    def cpv_of(value):
        if not isinstance(value, str) or catpkgsplit(value) is None:
            raise ValueError(f"not a cpv: {value!r}")
        return value

    if "uninstall" in fields:
        if "cpv" in fields:
            raise ValueError('a request has "cpv" or "uninstall", not both')
        clean_world = fields.get("clean_world", False)
        if not isinstance(clean_world, bool):
            raise ValueError('"clean_world" is true or false')
        return Uninstall(cpv_of(fields["uninstall"]), clean_world)
    cpv, repo = fields.get("cpv"), fields.get("repo")
    if not isinstance(cpv, str) or not isinstance(repo, str):
        raise ValueError('a request needs "cpv" and "repo" strings')
    blockers = fields.get("blockers", [])
    if not isinstance(blockers, list):
        raise ValueError('"blockers" is a list of cpvs')
    world = fields.get("world")
    if world is not None:
        try:
            Atom(world, allow_repo=True)
        except (InvalidAtom, TypeError):
            raise ValueError(f"not an atom: {world!r}") from None
    return Merge(cpv_of(cpv), repo, tuple(map(cpv_of, blockers)), world)


def event(**fields):
    """An event's line."""
    return json.dumps(fields, sort_keys=True) + "\n"


def _queries_see(trees):
    """has_version and best_version, asked by an ebuild, answered from trees: as emerge does,
    so that they see what this process merged."""
    from portage.package.ebuild._ipc.QueryCommand import QueryCommand

    QueryCommand._db = trees


class _Blocker(NamedTuple):
    """An installed package a merge blocks or is blocked by, as dblink takes it."""

    cpv: str
    slot_atom: object


def _edit_world(eroot, edit):
    """Calls edit with the world file's set, locked and loaded, as emerge edits it."""
    from portage._sets.files import WorldSelectedSet

    world = WorldSelectedSet(eroot)
    world.lock()
    try:
        world.load()
        edit(world)
    finally:
        world.unlock()


def _setcpv(settings, cpv, metadata, portdb):
    """settings set up for the ebuild of cpv with metadata, as from emerge's Package: its cpv
    knows its database, through which a phase finds the repositories' revisions to record.
    """
    from portage.versions import _pkg_str

    settings.setcpv(cpv, mydb=metadata)
    settings.mycpv = _pkg_str(cpv, metadata=metadata, settings=settings, db=portdb)


class Worker:
    """Merges into one root, as its configuration says, with what portage keeps between merges
    loaded once."""

    def __init__(self, config_root=None, root=None, eprefix=None):
        import portage

        from egraph_build.cli import portage_environment

        env = portage_environment(config_root, root, eprefix)
        options = {"config_root": config_root, "target_root": root, "eprefix": eprefix}
        trees = portage.create_trees(**options, env=env)
        self._settings = portage.config(**options, env=env)
        eroot = self._eroot = self._settings["EROOT"]
        self._portdb = trees[eroot]["porttree"].dbapi
        self._vartree = trees[eroot]["vartree"]
        # env-update's record of the library directories, which emerge keeps here too.
        self._mtimedb = portage.MtimeDB(
            os.path.join(eroot, portage.CACHE_PATH, "mtimedb")
        )
        _queries_see(trees)

    def _setup(self, request):
        """The request's ebuild and its configuration, as emerge's EbuildBuild sets them up;
        ValueError when it has no ebuild."""
        import portage
        from portage.exception import PortageKeyError

        ebuild = self._portdb.findname(request.cpv, myrepo=request.repo)
        if ebuild is None:
            raise ValueError(f"no ebuild of {request.cpv}::{request.repo}")
        keys = sorted(set(portage.auxdbkeys) | {"repository"})
        try:
            values = self._portdb.aux_get(request.cpv, keys, myrepo=request.repo)
        except PortageKeyError as e:
            raise ValueError(f"no metadata for {request.cpv}::{request.repo}: {e}")
        metadata = dict(zip(keys, values))
        settings = portage.config(clone=self._settings)
        _setcpv(settings, request.cpv, metadata, self._portdb)
        pkg = settings.configdict["pkg"]
        pkg["SRC_URI"] = metadata["SRC_URI"]
        pkg["EMERGE_FROM"] = "ebuild"
        pkg["MERGE_TYPE"] = "source"
        portage.doebuild_environment(
            ebuild, "setup", settings=settings, db=self._portdb
        )
        return ebuild, settings

    def merge(self, request, emit):
        """Builds and merges the request's package, emitting its events; whether it merged."""
        import portage

        try:
            ebuild, settings = self._setup(request)
            blockers = self._blockers(request.blockers)
        except ValueError as e:
            emit(event(error=str(e)))
            return False
        dbs = {"mydbapi": self._portdb, "vartree": self._vartree}
        for phase in PHASES:
            emit(event(phase=phase))
            status = portage.doebuild(
                ebuild, phase, settings=settings, tree="porttree", **dbs
            )
            if status != os.EX_OK:
                return self._failed(emit, phase, status, settings)
        emit(event(phase="merge"))
        status = portage.merge(
            settings["CATEGORY"],
            settings["PF"],
            settings["D"],
            os.path.join(settings["PORTAGE_BUILDDIR"], "build-info"),
            settings=settings,
            myebuild=ebuild,
            mytree="porttree",
            prev_mtimes=self._mtimedb["ldpath"],
            blockers=lambda: blockers,
            **dbs,
        )
        self._mtimedb.commit()
        if status != os.EX_OK:
            return self._failed(emit, "merge", status, settings)
        if request.world is not None:
            _edit_world(self._eroot, lambda world: world.add(request.world))
        emit(event(merged=request.cpv))
        return True

    def _blockers(self, cpvs):
        """The installed packages of cpvs as a merge takes its blockers; ValueError for one not
        installed."""
        from portage.dep import Atom
        from portage.versions import cpv_getkey

        vardb = self._vartree.dbapi
        found = []
        for cpv in cpvs:
            if not vardb.cpv_exists(cpv):
                raise ValueError(f"blocker {cpv} is not installed")
            (slot,) = vardb.aux_get(cpv, ["SLOT"])
            slot_atom = Atom(f"{cpv_getkey(cpv)}:{slot.partition('/')[0]}")
            found.append(_Blocker(cpv, slot_atom))
        return found

    def uninstall(self, request, emit):
        """Uninstalls the request's package as emerge's PackageUninstall does, emitting its
        events; whether it went."""
        import portage
        from portage.exception import UnsupportedAPIException

        vardb = self._vartree.dbapi
        if not vardb.cpv_exists(request.cpv):
            emit(event(error=f"{request.cpv} is not installed"))
            return False
        settings = portage.config(clone=self._settings)
        settings.setcpv(request.cpv, mydb=vardb)
        category, pf = portage.catsplit(request.cpv)
        ebuild = os.path.join(vardb.getpath(request.cpv), pf + ".ebuild")
        try:
            portage.doebuild_environment(ebuild, "prerm", settings=settings, db=vardb)
        except UnsupportedAPIException:
            pass
        portage.prepare_build_dirs(settings=settings, cleanup=True)
        emit(event(phase="unmerge"))
        status = portage.unmerge(
            category,
            pf,
            settings=settings,
            vartree=self._vartree,
            ldpath_mtimes=self._mtimedb["ldpath"],
        )
        self._mtimedb.commit()
        if status != os.EX_OK:
            return self._failed(emit, "unmerge", status, settings)
        if request.clean_world:
            _edit_world(
                self._eroot, lambda world: world.cleanPackage(vardb, request.cpv)
            )
        emit(event(uninstalled=request.cpv))
        return True

    def handle(self, request, emit):
        """Carries out a Merge or an Uninstall; whether it succeeded."""
        if isinstance(request, Uninstall):
            return self.uninstall(request, emit)
        return self.merge(request, emit)

    @staticmethod
    def _failed(emit, phase, status, settings):
        emit(
            event(failed=phase, status=status, log=settings.get("PORTAGE_LOG_FILE", ""))
        )
        return False


def serve(worker, lines, emit):
    """Each request in lines, in turn."""
    for line in lines:
        if not line.strip():
            continue
        try:
            request = parse_request(line)
        except ValueError as e:
            emit(event(error=str(e)))
            continue
        worker.handle(request, emit)


def main(config_root=None, root=None, eprefix=None):
    # The events keep stdout to themselves; what anything else writes there goes to stderr.
    sys.stdout.flush()
    events = os.fdopen(os.dup(sys.stdout.fileno()), "w")
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())

    def emit(line):
        events.write(line)
        events.flush()

    serve(Worker(config_root, root, eprefix), sys.stdin, emit)
    return 0
