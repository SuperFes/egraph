"""The kernel sources installed packages own, for egraph to keep the running kernel's."""

import json
import os


def sources(vartree, cpvs):
    """For each installed cpv, the directories straight under ${EPREFIX}/usr/src named linux-*
    that its CONTENTS lists, or the parents of what it lists (as dblink.isowner counts them),
    sorted, as paths under ROOT. KeyError for a cpv not installed."""
    import portage

    settings = vartree.settings
    root = settings["ROOT"]
    src = os.path.join(os.sep, settings["EPREFIX"].lstrip(os.sep), "usr", "src")
    found = {}
    for cpv in cpvs:
        if not vartree.dbapi.cpv_exists(cpv):
            raise KeyError(cpv)
        category, pf = portage.catsplit(cpv)
        link = portage.dblink(
            category, pf, myroot=root, settings=settings, vartree=vartree
        )
        dirs = set()
        for path, entry in link.getcontents().items():
            if entry[0] != "dir":
                continue
            under_root = os.path.join(os.sep, os.path.relpath(path, root))
            parent, name = os.path.split(under_root)
            if parent == src and name.startswith("linux-"):
                dirs.add(under_root)
        found[cpv] = sorted(dirs)
    return found


def to_json(found):
    return json.dumps(found, sort_keys=True)
