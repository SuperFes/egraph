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
        "--pending",
        dest="mode",
        action="store_const",
        const="pending",
        help="write what each ENTRY of a merge list waits for, as JSON, to --output",
    )
    p.add_argument(
        "--output",
        type=Path,
        help="file --pending writes, apart from anything portage prints",
    )
    p.add_argument(
        "entries",
        nargs="*",
        metavar="ENTRY",
        help="with --pending: a merge list entry, ebuild:CPV or binary:CPV",
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


def open_vardb(config_root, root, eprefix=None):
    import portage
    from portage.dbapi.vartree import vartree

    settings = portage.config(
        config_root=config_root, target_root=root, eprefix=eprefix
    )
    return vartree(settings=settings).dbapi


def _tree(config_root, root, eprefix):
    import portage

    trees = portage.create_trees(
        config_root=config_root, target_root=root, eprefix=eprefix
    )
    eroot = portage.config(config_root=config_root, target_root=root, eprefix=eprefix)[
        "EROOT"
    ]
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


def _previous(path):
    from egraph_build import store

    try:
        with open(path, "rb") as f:
            return store.decode(f.read())
    except (OSError, store.StoreError):
        return None


def write_store(args, incremental):
    import portage

    from egraph_build import __version__, build, installed, profile, store

    vardb, portdb = open_databases(args.config_root, args.root, args.eprefix)
    path = args.store or store.default_path(vardb.settings["EROOT"])
    previous = _previous(path) if incremental else None
    result = build.incremental(vardb, *previous) if previous else build.full(vardb)
    if not result.full and os.environ.get("EGRAPH_STRICT") == "1":
        expected = installed.to_json(build.full(vardb).layer)
        if installed.to_json(result.layer) != expected:
            print(
                "egraph-build: EGRAPH_STRICT: incremental build differs from a full build",
                file=sys.stderr,
            )
            return EXIT_FAILURE
    meta = store.Meta(
        egraph_version=__version__,
        portage_version=portage.VERSION,
        eroot=vardb.settings["EROOT"],
        build_time_ns=result.started_ns,
        implicit=profile.implicit_iuse(vardb.settings),
    )
    store.write(path, store.encode(result.layer, meta, result.inputs))
    ev = build.evaluate(vardb, portdb)
    if ev.layer.installed() != result.layer.installed():
        print(
            "egraph-build: the installed packages changed during the build",
            file=sys.stderr,
        )
        return EXIT_FAILURE
    evaluated_meta = store.EvaluatedMeta(
        egraph_version=__version__,
        portage_version=portage.VERSION,
        eroot=vardb.settings["EROOT"],
        build_time_ns=ev.started_ns,
        installed_build_time_ns=result.started_ns,
    )
    store.write(
        store.evaluated_path(path),
        store.encode_evaluated(ev.layer, evaluated_meta, ev.inputs),
    )
    return EXIT_OK


def main(argv=None):
    try:
        args = parser().parse_args(argv)
    except SystemExit as e:
        return EXIT_OK if e.code == 0 else EXIT_USAGE
    if args.mode == "pending":
        return write_pending(args)
    if args.entries:
        print("egraph-build: entries are for --pending", file=sys.stderr)
        return EXIT_USAGE
    if args.mode == "json":
        from egraph_build import build, installed

        vardb = open_vardb(args.config_root, args.root, args.eprefix)
        sys.stdout.write(installed.to_json(build.full(vardb).layer))
        return EXIT_OK
    if args.mode == "evaluated-json":
        from egraph_build import evaluated

        vardb, portdb = open_databases(args.config_root, args.root, args.eprefix)
        sys.stdout.write(evaluated.to_json(evaluated.build(vardb, portdb)))
        return EXIT_OK
    return write_store(args, incremental=args.mode == "incremental")
