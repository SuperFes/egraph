"""Which dependency strings emerge reads for an installed package under --dynamic-deps=y.

This is emerge's FakeVartree rule, kept in one place for the builder and the oracle alike;
test_dynamic_deps.py holds it to FakeVartree itself.
"""

import os

from portage.dep import Atom, use_reduce
from portage.exception import InvalidAtom, InvalidDependString

from egraph_build.model import DEP_KINDS

SOURCES = ("ebuild", "vdb", "moved")


def _atoms(tokens, out):
    for token in tokens:
        if isinstance(token, list):
            _atoms(token, out)
        elif token != "||":
            out.append(token)


def _built_slot_operator_atoms(vardb, cpv):
    """The installed package's := atoms as built (:SLOT/SUBSLOT=), by kind."""
    keys = list(DEP_KINDS) + ["USE", "EAPI"]
    metadata = dict(zip(keys, vardb.aux_get(cpv, keys)))
    use = frozenset(metadata["USE"].split())
    found = {}
    for kind in DEP_KINDS:
        try:
            tokens = use_reduce(
                metadata[kind],
                uselist=use,
                eapi=metadata["EAPI"] or None,
                opconvert=False,
                token_class=Atom,
            )
        except (InvalidAtom, InvalidDependString):
            continue
        atoms = []
        _atoms(tokens, atoms)
        built = [str(atom) for atom in atoms if atom.slot_operator_built]
        if built:
            found[kind] = built
    return found


def global_updates(portdb):
    """Every repository's package moves (profiles/updates), and the main repository's as DEFAULT."""
    from portage.exception import DirectoryNotFound
    from portage.update import grab_updates, parse_updates

    found = {}
    for name in portdb.getRepositories():
        path = os.path.join(portdb.getRepositoryPath(name), "profiles", "updates")
        if not os.path.isdir(path):
            continue
        try:
            raw = grab_updates(path)
        except DirectoryNotFound:
            raw = []
        commands = []
        for _, _, content in raw:
            commands.extend(parse_updates(content)[0])
        found[name] = commands
    main = portdb.repositories.mainRepo()
    if main is not None and main.name in found:
        found["DEFAULT"] = found[main.name]
    return found


def _moved(vardb, cpv, strings, updates):
    """The vdb's dependency strings with the package's repository's moves applied."""
    from portage.update import update_dbentries

    # update_dbentries takes the package as a _pkg_str; this is the one place egraph builds one.
    from portage.versions import _pkg_str

    eapi, slot, repo = vardb.aux_get(cpv, ["EAPI", "SLOT", "repository"])
    commands = updates.get(repo) or updates.get("DEFAULT")
    if not commands:
        return strings
    parent = _pkg_str(
        cpv,
        metadata={"EAPI": eapi, "SLOT": slot, "repository": repo},
        settings=vardb.settings,
    )
    changed = update_dbentries(commands, dict(strings), parent=parent)
    return {kind: changed.get(kind, strings[kind]) for kind in DEP_KINDS}


def dependency_strings(vardb, portdb, cpv, updates=None):
    """(source, {kind: dependency string}, EAPI) as emerge's default --dynamic-deps=y sees cpv.

    source is "ebuild" when the same version is in the package's repository and portage supports
    both EAPIs: the ebuild's strings replace the vdb's, with the built := atoms appended, as
    emerge's FakeVartree does, and the ebuild's EAPI is the one to read them with. Otherwise it
    is "vdb", with the repositories' package moves applied ("moved" when one changed anything).
    updates is global_updates(portdb), read here when not given.
    """
    from portage import eapi_is_supported
    from portage.eapi import eapi_has_slot_operator
    from portage.exception import PortageException

    installed_eapi, repo = vardb.aux_get(cpv, ["EAPI", "repository"])
    strings = dict(zip(DEP_KINDS, vardb.aux_get(cpv, list(DEP_KINDS))))
    try:
        live = dict(
            zip(
                DEP_KINDS + ("EAPI",),
                portdb.aux_get(cpv, list(DEP_KINDS) + ["EAPI"], myrepo=repo or None),
            )
        )
    except (KeyError, PortageException):
        live = None
    if (
        live is not None
        and eapi_is_supported(live["EAPI"])
        and eapi_is_supported(installed_eapi)
    ):
        built = (
            _built_slot_operator_atoms(vardb, cpv)
            if eapi_has_slot_operator(installed_eapi)
            else {}
        )
        if not built or eapi_has_slot_operator(live["EAPI"]):
            for kind, atoms in built.items():
                live[kind] += " " + " ".join(atoms)
            return "ebuild", {kind: live[kind] for kind in DEP_KINDS}, live["EAPI"]
    if updates is None:
        updates = global_updates(portdb)
    moved = _moved(vardb, cpv, strings, updates)
    return ("moved" if moved != strings else "vdb"), moved, installed_eapi
