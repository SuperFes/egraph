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
    p.set_defaults(mode="full")
    return p


def main(argv=None):
    try:
        args = parser().parse_args(argv)
    except SystemExit as e:
        return EXIT_OK if e.code == 0 else EXIT_USAGE
    print(f"egraph-build: {args.mode}: not implemented", file=sys.stderr)
    return EXIT_NOT_IMPLEMENTED
