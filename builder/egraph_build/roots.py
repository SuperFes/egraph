"""Root sets: the packages depclean keeps whatever depends on them; and every other set.

@selected (the world file and world_sets), @system and @profile, as portage's set
configuration expands them; the others listed, and expanded as emerge's arguments, on
demand. Sets live in portage's private _sets package; this is the one place that uses it, so
drift in portage breaks this module and nothing else.
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
    # For an atom of @selected, the first set world_sets names that holds it, which depclean
    # keeps when its arguments empty @selected; "" for any other.
    via: str = ""


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
    """Set name -> {atom: via}, nested sets expanded."""
    config = _set_config(vardb)
    atoms = {}
    for name in ROOT_SETS:
        try:
            atoms[name] = dict.fromkeys(config.getSetAtoms(name), "")
        except PackageSetNotFound:
            # As depclean: a set naming a missing set keeps its own atoms.
            atoms[name] = dict.fromkeys(config.getSets()[name].getAtoms(), "")
            continue
        if name != "selected":
            continue
        nested = sorted(
            set_name[1:] for set_name in config.getSets()[name].getNonAtoms()
        )
        for set_name in reversed(nested):
            for atom in config.getSetAtoms(set_name):
                atoms[name][atom] = set_name
    return atoms


class SetInfo(NamedTuple):
    name: str
    # The class portage builds it with, as its name.
    kind: str
    description: str
    # Whether emerge records it in world_sets when asked for it without --oneshot.
    world_candidate: bool


class SetAtoms(NamedTuple):
    name: str
    # (set, atom) as emerge's arguments hold them: a nested set's atoms under its own name.
    atoms: tuple = ()
    # Why emerge could not load it; None when it could.
    error: object = None


def set_list(vardb):
    """Every set portage's set configuration defines, by name, and the errors it met."""
    config = _set_config(vardb)
    sets = config.getSets()
    return (
        tuple(
            SetInfo(
                name,
                type(sets[name]).__name__,
                sets[name].getMetadata("DESCRIPTION"),
                bool(sets[name].world_candidate),
            )
            for name in sorted(sets)
        ),
        tuple(config.errors),
    )


def _expanded(config, name):
    """As depgraph's _expand_set_args: the set, then each nested set it reaches, depth first
    through each one's nested sets in sorted order, each once."""
    sets = config.getSets()
    found, seen, stack = [], set(), [name]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        pset = sets[current]
        found.extend((current, str(atom)) for atom in sorted(pset.getAtoms(), key=str))
        for token in sorted(pset.getNonAtoms()):
            if token.startswith("@") and token[1:] in sets:
                stack.append(token[1:])
    return tuple(found)


def set_atoms(vardb, names):
    """A SetAtoms for each name, in the order given."""
    from portage.exception import PortageException

    config = _set_config(vardb)
    found = []
    for name in names:
        if name not in config.getSets():
            found.append(SetAtoms(name, (), "no such set"))
            continue
        try:
            # Raises for a nested set that does not exist, as emerge refuses it.
            config.getSetAtoms(name)
            found.append(SetAtoms(name, _expanded(config, name)))
        except PackageSetNotFound as e:
            found.append(SetAtoms(name, (), f"it names a set that does not exist: {e}"))
        except PortageException as e:
            found.append(SetAtoms(name, (), f"{type(e).__name__}: {e}"))
    return tuple(found)


def read_roots(vardb, match):
    """Every root atom with the installed packages it matches, sets in ROOT_SETS order."""
    atoms = root_atoms(vardb)
    return tuple(
        Root(name, str(atom), match(atom), atoms[name][atom])
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
