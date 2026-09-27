"""Root sets: the packages depclean keeps whatever depends on them.

@selected (the world file and world_sets), @system and @profile, as portage's set
configuration expands them. Sets live in portage's private _sets package; this is the one
place that uses it, so drift in portage breaks this module and nothing else.
"""

import os
from typing import NamedTuple

import portage
from portage.const import GLOBAL_CONFIG_PATH, USER_CONFIG_PATH, WORLD_FILE
from portage.const import WORLD_SETS_FILE
from portage.exception import PackageSetNotFound

# The sets depclean starts from, in the order it names them.
ROOT_SETS = ("selected", "system", "profile")


class Root(NamedTuple):
    set: str
    atom: str
    # Installed cpvs the atom matches.
    matches: tuple


def _set_config(vardb):
    from portage._sets import load_default_config
    from portage.dbapi.bintree import binarytree
    from portage.dbapi.porttree import portagetree
    from portage.util import LazyItemsDict

    settings = vardb.settings
    # As portage.create_trees, around the vartree we already have. Set classes that the
    # default configuration defines but nothing uses never load the others.
    trees = LazyItemsDict()
    trees["vartree"] = vardb.vartree
    trees.addLazySingleton("porttree", portagetree, settings=settings)
    trees.addLazySingleton(
        "bintree", binarytree, pkgdir=settings["PKGDIR"], settings=settings
    )
    return load_default_config(settings, trees)


def root_atoms(vardb):
    """Set name -> atoms, nested sets expanded."""
    config = _set_config(vardb)
    atoms = {}
    for name in ROOT_SETS:
        try:
            atoms[name] = config.getSetAtoms(name)
        except PackageSetNotFound:
            # As depclean: a set naming a missing set keeps its own atoms.
            atoms[name] = config.getSets()[name].getAtoms()
    return atoms


def read_roots(vardb, match):
    """Every root atom with the installed packages it matches, sets in ROOT_SETS order."""
    atoms = root_atoms(vardb)
    return tuple(
        Root(name, str(atom), match(atom))
        for name in ROOT_SETS
        for atom in sorted(atoms[name], key=str)
    )


def _entries(directory):
    found = []
    for path, dirs, files in os.walk(directory):
        dirs.sort()
        found.extend(os.path.join(path, d) for d in dirs)
        found.extend(os.path.join(path, f) for f in sorted(files))
    return found


def input_paths(settings):
    """Files the root sets are read from, other than the profile's.

    The same list portage's set configuration reads, plus the world files and the user sets
    directory its default configuration points at.
    """
    eroot = settings["EROOT"]
    user = os.path.join(settings["PORTAGE_CONFIGROOT"], USER_CONFIG_PATH)
    global_sets = os.path.join(
        portage.const.EPREFIX, GLOBAL_CONFIG_PATH.lstrip(os.sep), "sets"
    )
    paths = [
        os.path.join(eroot, WORLD_FILE),
        os.path.join(eroot, WORLD_SETS_FILE),
        global_sets,
        os.path.join(user, "sets.conf"),
        os.path.join(user, "sets"),
    ]
    paths.extend(
        os.path.join(repo.location, "sets.conf") for repo in settings.repositories
    )
    for directory in (
        global_sets,
        os.path.join(user, "sets.conf"),
        os.path.join(user, "sets"),
    ):
        paths.extend(_entries(directory))
    return paths
