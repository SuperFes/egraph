import argparse
import os
import sys
from pathlib import Path

# Shared with the egraph binary's Exit enum.
EXIT_OK = 0
EXIT_FAILURE = 1
EXIT_USAGE = 2
EXIT_NOT_IMPLEMENTED = 3


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
    p.add_argument(
        "--store",
        type=Path,
        default=os.environ.get("EGRAPH_STORE"),
        help="store file to write (default: ${EROOT}/var/cache/egraph/installed.egraph)",
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


def _previous(path):
    from egraph_build import store

    try:
        with open(path, "rb") as f:
            return store.decode(f.read())
    except (OSError, store.StoreError):
        return None


def write_store(args, incremental):
    import portage

    from egraph_build import __version__, build, installed, store

    vardb = open_vardb(args.config_root, args.root, args.eprefix)
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
    )
    store.write(path, store.encode(result.layer, meta, result.inputs))
    return EXIT_OK


def main(argv=None):
    try:
        args = parser().parse_args(argv)
    except SystemExit as e:
        return EXIT_OK if e.code == 0 else EXIT_USAGE
    if args.mode == "json":
        from egraph_build import build, installed

        vardb = open_vardb(args.config_root, args.root, args.eprefix)
        sys.stdout.write(installed.to_json(build.full(vardb).layer))
        return EXIT_OK
    return write_store(args, incremental=args.mode == "incremental")
