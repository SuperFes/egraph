"""Builds and merges packages one at a time, as emerge builds one from source, for egraph-exec.

Requests come one per line on stdin, as JSON: {"cpv": ..., "repo": ...}. For each, events go to
stdout one per line: {"phase": name} as each phase starts, then {"merged": cpv}, or
{"failed": phase, "status": n, "log": path} when a phase fails, or {"error": message} for a
request that cannot be tried. Whatever portage or an ebuild prints goes to stderr or the build
log, never among the events.
"""

import json
import os
import sys
from typing import NamedTuple

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


class Request(NamedTuple):
    cpv: str
    repo: str


def parse_request(line):
    """A request from its line; ValueError for anything else."""
    from portage.versions import catpkgsplit

    try:
        fields = json.loads(line)
    except json.JSONDecodeError as e:
        raise ValueError(f"not JSON: {e}") from None
    if not isinstance(fields, dict):
        raise ValueError("not a JSON object")
    cpv, repo = fields.get("cpv"), fields.get("repo")
    if not isinstance(cpv, str) or not isinstance(repo, str):
        raise ValueError('a request needs "cpv" and "repo" strings')
    if catpkgsplit(cpv) is None:
        raise ValueError(f"not a cpv: {cpv}")
    return Request(cpv, repo)


def event(**fields):
    """An event's line."""
    return json.dumps(fields, sort_keys=True) + "\n"


def _queries_see(trees):
    """has_version and best_version, asked by an ebuild, answered from trees: as emerge does,
    so that they see what this process merged."""
    from portage.package.ebuild._ipc.QueryCommand import QueryCommand

    QueryCommand._db = trees


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
        eroot = self._settings["EROOT"]
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
        status = portage.merge(
            settings["CATEGORY"],
            settings["PF"],
            settings["D"],
            os.path.join(settings["PORTAGE_BUILDDIR"], "build-info"),
            settings=settings,
            myebuild=ebuild,
            mytree="porttree",
            prev_mtimes=self._mtimedb["ldpath"],
            **dbs,
        )
        self._mtimedb.commit()
        if status != os.EX_OK:
            return self._failed(emit, "merge", status, settings)
        emit(event(merged=request.cpv))
        return True

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
        worker.merge(request, emit)


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
