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
        help="store file to write",
    )
    p.add_argument(
        "--root",
        default=os.environ.get("ROOT", "/"),
        help="root whose installed packages to evaluate",
    )
    p.add_argument(
        "--config-root",
        default=os.environ.get("PORTAGE_CONFIGROOT", "/"),
        help="root of the portage configuration to evaluate them with",
    )
    p.set_defaults(mode="full")
    return p


def open_vardb(config_root, root):
    import portage
    from portage.dbapi.vartree import vartree

    settings = portage.config(config_root=config_root, target_root=root)
    return vartree(settings=settings).dbapi


def write_store(args):
    import time

    import portage

    from egraph_build import __version__, installed, store

    vardb = open_vardb(args.config_root, args.root)
    layer = installed.build(vardb)
    meta = store.Meta(
        egraph_version=__version__,
        portage_version=portage.VERSION,
        eroot=vardb.settings["EROOT"],
        build_time_ns=time.time_ns(),
    )
    path = args.store or store.default_path(args.root)
    store.write(path, store.encode(layer, meta))


def main(argv=None):
    try:
        args = parser().parse_args(argv)
    except SystemExit as e:
        return EXIT_OK if e.code == 0 else EXIT_USAGE
    if args.mode == "json":
        from egraph_build import installed

        layer = installed.build(open_vardb(args.config_root, args.root))
        sys.stdout.write(installed.to_json(layer))
        return EXIT_OK
    if args.mode == "full":
        write_store(args)
        return EXIT_OK
    print(f"egraph-build: {args.mode}: not implemented", file=sys.stderr)
    return EXIT_NOT_IMPLEMENTED
