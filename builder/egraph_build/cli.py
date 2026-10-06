import argparse
import os
import sys
from pathlib import Path

# Shared with the egraph binary's Exit enum.
EXIT_OK = 0
EXIT_FAILURE = 1
EXIT_USAGE = 2
EXIT_NOT_IMPLEMENTED = 3
# egraph check: the store differs from a fresh build.
EXIT_DRIFT = 4
# --verify: emerge --pretend would merge otherwise.
EXIT_DIFFERS = 5
# updates and plan: emerge would refuse the plan for its blockers.
EXIT_REFUSED = 6


def parser():
    p = argparse.ArgumentParser(
        prog="egraph-build",
        description="Evaluate the installed packages through portage and write the store.",
    )
    mode = p.add_mutually_exclusive_group()
    mode.add_argument(
        "--full",
        dest="mode",
        action="store_const",
        const="full",
        help="evaluate every installed package (default)",
    )
    mode.add_argument(
        "--incremental",
        dest="mode",
        action="store_const",
        const="incremental",
        help="re-evaluate only the packages whose inputs changed",
    )
    mode.add_argument(
        "--evaluate",
        dest="mode",
        action="store_const",
        const="evaluate",
        help="as --incremental, also evaluating each ENTRY, a cp, until the next --full",
    )
    mode.add_argument(
        "--json",
        dest="mode",
        action="store_const",
        const="json",
        help="print the canonical JSON of the installed layer instead of writing the store",
    )
    mode.add_argument(
        "--evaluated-json",
        dest="mode",
        action="store_const",
        const="evaluated-json",
        help="print the canonical JSON of the evaluated layer instead of writing the store",
    )
    mode.add_argument(
        "--repository",
        dest="mode",
        action="store_const",
        const="repository",
        help="write the repository index beside the store: every version in the repositories, "
        "and what decides their visibility",
    )
    mode.add_argument(
        "--repository-json",
        dest="mode",
        action="store_const",
        const="repository-json",
        help="print the canonical JSON of the repository index instead of writing it",
    )
    mode.add_argument(
        "--pending",
        dest="mode",
        action="store_const",
        const="pending",
        help="write what each ENTRY of a merge list waits for, as JSON, to --output",
    )
    mode.add_argument(
        "--emerge-options",
        dest="mode",
        action="store_const",
        const="emerge-options",
        help="write EMERGE_DEFAULT_OPTS as emerge splits it, and where the elog summary goes, as "
        "JSON, to --output",
    )
    mode.add_argument(
        "--notices",
        dest="mode",
        action="store_const",
        const="notices",
        help="write the configuration updates waiting, the unread news and the preserved "
        "libraries, as JSON, to --output",
    )
    mode.add_argument(
        "--kernel-sources",
        dest="mode",
        action="store_const",
        const="kernel-sources",
        help="write the directories under /usr/src named linux-* that each ENTRY, an installed "
        "cpv, owns, as JSON, to --output",
    )
    mode.add_argument(
        "--copy-portage",
        dest="mode",
        action="store_const",
        const="copy-portage",
        help="copy the running portage to --output, a directory, for workers to run from while "
        "a run merges a new one",
    )
    mode.add_argument(
        "--worker",
        dest="mode",
        action="store_const",
        const="worker",
        help="build and merge, or uninstall, each package requested on stdin, one JSON "
        "request a line, reporting on stdout",
    )
    p.add_argument(
        "--background",
        action="store_true",
        help="with --worker: what builds and merges print goes only to their logs, as with "
        "emerge running more than one job",
    )
    p.add_argument(
        "--portage-copy",
        type=Path,
        help="with --worker: run on the copy of portage --copy-portage made there",
    )
    p.add_argument(
        "--output",
        type=Path,
        help="file --pending, --kernel-sources, --emerge-options and --notices write, apart from "
        "anything portage "
        "prints; the directory --copy-portage makes",
    )
    p.add_argument(
        "entries",
        nargs="*",
        metavar="ENTRY",
        help="with --pending: a merge list entry, ebuild:CPV or binary:CPV; with "
        "--evaluate: a cp in a repository",
    )
    p.add_argument(
        "--store",
        type=Path,
        default=os.environ.get("EGRAPH_STORE"),
        help="store file to write (default: ${EROOT}/var/cache/egraph/installed.egraph); "
        "the evaluated store goes beside it",
    )
    # Unset options fall through to portage's own defaults and environment.
    p.add_argument(
        "--root",
        default=os.environ.get("ROOT"),
        help="root whose installed packages to evaluate",
    )
    p.add_argument(
        "--config-root",
        default=os.environ.get("PORTAGE_CONFIGROOT"),
        help="root of the portage configuration to evaluate them with",
    )
    p.add_argument(
        "--eprefix",
        default=os.environ.get("PORTAGE_OVERRIDE_EPREFIX"),
        help="offset prefix of a prefix installation",
    )
    p.set_defaults(mode="full")
    return p


# What portage takes from the environment over its configuration files, besides the USE_EXPAND
# variables themselves.
_CONFIGURATION = frozenset(
    (
        "USE",
        "USE_EXPAND",
        "USE_EXPAND_UNPREFIXED",
        "ACCEPT_KEYWORDS",
        "ACCEPT_LICENSE",
        "ACCEPT_PROPERTIES",
        "ACCEPT_RESTRICT",
    )
)


def portage_environment(config_root, root, eprefix=None, environ=None):
    """environ (os.environ by default) without what would override portage's configuration
    files: a hook portage runs inherits its whole configuration, and USE or POSTGRES_TARGETS
    taken back from there outrank package.use."""
    import portage

    env = {
        name: value
        for name, value in (os.environ if environ is None else environ).items()
        if name not in _CONFIGURATION
    }
    probe = portage.config(
        config_root=config_root, target_root=root, eprefix=eprefix, env=env
    )
    expanded = set(probe.get("USE_EXPAND", "").split())
    expanded.update(probe.get("USE_EXPAND_UNPREFIXED", "").split())
    return {name: value for name, value in env.items() if name not in expanded}


def open_vardb(config_root, root, eprefix=None):
    import portage
    from portage.dbapi.vartree import vartree

    settings = portage.config(
        config_root=config_root,
        target_root=root,
        eprefix=eprefix,
        env=portage_environment(config_root, root, eprefix),
    )
    return vartree(settings=settings).dbapi


def _tree(config_root, root, eprefix):
    import portage

    env = portage_environment(config_root, root, eprefix)
    trees = portage.create_trees(
        config_root=config_root, target_root=root, eprefix=eprefix, env=env
    )
    eroot = portage.config(
        config_root=config_root, target_root=root, eprefix=eprefix, env=env
    )["EROOT"]
    return trees[eroot]


def open_databases(config_root, root, eprefix=None):
    """The installed and the ebuild package databases for a root."""
    tree = _tree(config_root, root, eprefix)
    return tree["vartree"].dbapi, tree["porttree"].dbapi


def open_trees(config_root, root, eprefix=None):
    """The configuration, and the ebuild and binary package databases for its root."""
    tree = _tree(config_root, root, eprefix)
    return tree["vartree"].settings, tree["porttree"].dbapi, tree["bintree"].dbapi


def write_pending(args):
    from egraph_build import pending

    if args.output is None:
        print("egraph-build: --pending needs --output", file=sys.stderr)
        return EXIT_USAGE
    try:
        entries = [pending.parse_entry(entry) for entry in args.entries]
    except ValueError as e:
        print(f"egraph-build: {e}", file=sys.stderr)
        return EXIT_USAGE
    settings, portdb, bindb = open_trees(args.config_root, args.root, args.eprefix)
    result = pending.waits(settings, portdb, bindb, entries)
    args.output.write_text(pending.to_json(result))
    return EXIT_OK


def write_kernel_sources(args):
    from egraph_build import kernel

    if args.output is None:
        print("egraph-build: --kernel-sources needs --output", file=sys.stderr)
        return EXIT_USAGE
    vartree = _tree(args.config_root, args.root, args.eprefix)["vartree"]
    try:
        found = kernel.sources(vartree, args.entries)
    except KeyError as e:
        print(f"egraph-build: {e.args[0]} is not installed", file=sys.stderr)
        return EXIT_USAGE
    args.output.write_text(kernel.to_json(found))
    return EXIT_OK


def jobserver(settings):
    """The named pipe emerge takes a token from for each build under FEATURES=jobserver-token:
    the last --jobserver-auth in MAKEFLAGS, if it names one; None otherwise."""
    if "jobserver-token" not in settings.get("FEATURES", "").split():
        return None
    path = None
    for flag in settings.get("MAKEFLAGS", "").split():
        if flag.startswith("--jobserver-auth="):
            value = flag[len("--jobserver-auth=") :]
            path = value[len("fifo:") :] if value.startswith("fifo:") else None
    return path


def write_emerge_options(args):
    import json
    import shlex

    import portage
    from portage import installation

    from egraph_build import notices

    if args.output is None:
        print("egraph-build: --emerge-options needs --output", file=sys.stderr)
        return EXIT_USAGE
    # The whole environment, as emerge reads it.
    settings = portage.config(
        config_root=args.config_root, target_root=args.root, eprefix=args.eprefix
    )
    summary, system = notices.elog_settings(settings)
    written = {
        "options": shlex.split(settings.get("EMERGE_DEFAULT_OPTS", "")),
        "elog": {"summary": summary, "system": system},
        "jobserver": jobserver(settings),
        "portage_installed": installation.TYPE == installation.TYPES.SYSTEM,
        "merge_wait": "merge-wait" in settings.features,
        "tmpdir": settings["PORTAGE_TMPDIR"],
    }
    args.output.write_text(json.dumps(written, indent=1, sort_keys=True))
    return EXIT_OK


def write_notices(args):
    from egraph_build import notices

    if args.output is None:
        print("egraph-build: --notices needs --output", file=sys.stderr)
        return EXIT_USAGE
    tree = _tree(args.config_root, args.root, args.eprefix)
    vardb = tree["vartree"].dbapi
    settings = vardb.settings
    args.output.write_text(
        notices.to_json(
            notices.config_updates(settings),
            notices.unread_news(settings, tree["porttree"].dbapi),
            *notices.preserved_libraries(vardb),
        )
    )
    return EXIT_OK


def _previous(path, decode):
    from egraph_build import store

    try:
        with open(path, "rb") as f:
            return decode(f.read())
    except (OSError, store.StoreError):
        return None


def _strict_failure(what):
    print(
        f"egraph-build: EGRAPH_STRICT: incremental build of the {what} differs from a full "
        "build",
        file=sys.stderr,
    )
    return EXIT_FAILURE


def _refusal(portdb, words):
    """Why words are not all cps with ebuilds in portdb's repositories, or None."""
    from portage.dep import Atom, isvalidatom

    known = frozenset(portdb.cp_all())
    for word in words:
        if not isvalidatom(word) or Atom(word).cp != word:
            return f"{word}: not a category/package name"
        if word not in known:
            return f"{word}: no ebuilds in the repositories"
    return None


def write_store(args, incremental, requested=()):
    import portage

    from egraph_build import __version__, build, evaluated, installed, profile, store

    vardb, portdb = open_databases(args.config_root, args.root, args.eprefix)
    refusal = _refusal(portdb, requested)
    if refusal:
        print(f"egraph-build: {refusal}", file=sys.stderr)
        return EXIT_USAGE
    strict = os.environ.get("EGRAPH_STRICT") == "1"
    path = args.store or store.default_path(vardb.settings["EROOT"])
    evaluated_path = store.evaluated_path(path)
    previous = _previous(path, store.decode) if incremental else None
    previous_evaluated = (
        _previous(evaluated_path, store.decode_evaluated) if previous else None
    )
    result = build.incremental(vardb, *previous) if previous else build.full(vardb)
    if not result.full and strict:
        expected = installed.to_json(build.full(vardb).layer)
        if installed.to_json(result.layer) != expected:
            return _strict_failure("installed store")
    if previous_evaluated:
        ev = build.evaluate_incremental(
            vardb,
            portdb,
            previous_evaluated,
            result,
            previous[0].build_time_ns,
            requested=requested,
        )
    else:
        ev = build.evaluate(vardb, portdb, requested)
    if ev.layer.installed() != result.layer.installed():
        print(
            "egraph-build: the installed packages changed during the build",
            file=sys.stderr,
        )
        return EXIT_FAILURE
    if not ev.full and strict:
        expected = evaluated.to_json(
            build.evaluate(vardb, portdb, ev.layer.requested()).layer
        )
        if evaluated.to_json(ev.layer) != expected:
            return _strict_failure("evaluated store")
    meta = store.Meta(
        egraph_version=__version__,
        portage_version=portage.VERSION,
        eroot=vardb.settings["EROOT"],
        build_time_ns=result.started_ns,
        implicit=profile.implicit_iuse(vardb.settings),
    )
    evaluated_meta = store.EvaluatedMeta(
        egraph_version=__version__,
        portage_version=portage.VERSION,
        eroot=vardb.settings["EROOT"],
        build_time_ns=ev.started_ns,
        installed_build_time_ns=result.started_ns,
    )
    store.write(path, store.encode(result.layer, meta, result.inputs))
    store.write(
        evaluated_path, store.encode_evaluated(ev.layer, evaluated_meta, ev.inputs)
    )
    return EXIT_OK


def write_repository(args):
    import portage

    from egraph_build import __version__, build, store

    vardb, portdb = open_databases(args.config_root, args.root, args.eprefix)
    eroot = vardb.settings["EROOT"]
    path = args.store or store.default_path(eroot)
    result = build.index(portdb)
    meta = store.RepositoryMeta(__version__, portage.VERSION, eroot, result.started_ns)
    store.write(
        store.repository_path(path),
        store.encode_repository(result.index, meta, result.inputs),
    )
    return EXIT_OK


def main(argv=None):
    try:
        args = parser().parse_args(argv)
    except SystemExit as e:
        return EXIT_OK if e.code == 0 else EXIT_USAGE
    if args.mode == "pending":
        return write_pending(args)
    if args.mode == "kernel-sources":
        return write_kernel_sources(args)
    if args.mode == "emerge-options":
        return write_emerge_options(args)
    if args.mode == "notices":
        return write_notices(args)
    if args.mode == "evaluate":
        if not args.entries:
            print("egraph-build: --evaluate takes the cps to evaluate", file=sys.stderr)
            return EXIT_USAGE
        return write_store(args, incremental=True, requested=args.entries)
    if args.entries:
        print(
            "egraph-build: entries are for --pending, --kernel-sources and --evaluate",
            file=sys.stderr,
        )
        return EXIT_USAGE
    if args.mode == "copy-portage":
        from egraph_build import selfupdate

        if args.output is None:
            print("egraph-build: --copy-portage needs --output", file=sys.stderr)
            return EXIT_USAGE
        try:
            selfupdate.copy_portage(str(args.output))
        except OSError as e:
            print(f"egraph-build: {args.output}: {e}", file=sys.stderr)
            return EXIT_FAILURE
        return EXIT_OK
    if args.mode == "worker":
        from egraph_build import worker

        if args.portage_copy is not None:
            from egraph_build import selfupdate

            selfupdate.use_copy(str(args.portage_copy))
        return worker.main(args.config_root, args.root, args.eprefix, args.background)
    if args.mode == "json":
        from egraph_build import build, installed

        vardb = open_vardb(args.config_root, args.root, args.eprefix)
        sys.stdout.write(installed.to_json(build.full(vardb).layer))
        return EXIT_OK
    if args.mode == "repository":
        return write_repository(args)
    if args.mode == "repository-json":
        from egraph_build import repository

        _, portdb = open_databases(args.config_root, args.root, args.eprefix)
        sys.stdout.write(repository.to_json(repository.read(portdb)))
        return EXIT_OK
    if args.mode == "evaluated-json":
        from egraph_build import evaluated

        vardb, portdb = open_databases(args.config_root, args.root, args.eprefix)
        sys.stdout.write(evaluated.to_json(evaluated.build(vardb, portdb)))
        return EXIT_OK
    return write_store(args, incremental=args.mode == "incremental")
