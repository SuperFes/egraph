"""mtimedb's resume entry, where emerge keeps the merges a run has left: saved as the run starts
and as --keep-going goes on, and each package dropped once merged, as emerge's scheduler does.
"""

import json
import os


def mtimedb_path(eroot):
    """Where emerge keeps its mtimedb under eroot."""
    import portage

    return os.path.join(eroot, portage.CACHE_PATH, "mtimedb")


def parse_entry(text):
    """The resume entry text holds, shaped as emerge reads one; ValueError otherwise."""
    try:
        entry = json.loads(text)
    except json.JSONDecodeError as e:
        raise ValueError(f"not JSON: {e}") from None
    if not isinstance(entry, dict):
        raise ValueError("not a JSON object")
    if not isinstance(entry.get("myopts"), dict):
        raise ValueError('"myopts" is an object')
    for key in ("favorites", "binpkgs"):
        if not isinstance(entry.get(key), list):
            raise ValueError(f'"{key}" is a list')
    mergelist = entry.get("mergelist")
    if not isinstance(mergelist, list) or not all(
        isinstance(task, list)
        and len(task) == 4
        and all(isinstance(field, str) for field in task)
        for task in mergelist
    ):
        raise ValueError('"mergelist" is a list of [type, root, cpv, operation]')
    return entry


def save(path, entry, backup):
    """Saves entry as the mtimedb's at path, as emerge does as a run goes on, or with backup as
    it starts, keeping a list of more than one merge from before as resume_backup."""
    import portage

    mtimedb = portage.MtimeDB(path)
    previous = mtimedb.get("resume")
    if (
        backup
        and isinstance(previous, dict)
        and isinstance(previous.get("mergelist"), list)
        and len(previous["mergelist"]) > 1
    ):
        mtimedb["resume_backup"] = previous
    mtimedb["resume"] = entry
    mtimedb.commit()


def drop(mtimedb, eroot, cpv):
    """Drops the ebuild of cpv merged into eroot from mtimedb's resume entry, and the entry once
    it lists nothing, for the caller to commit, as emerge's scheduler does after each merge.
    """
    resume = mtimedb.get("resume")
    if not isinstance(resume, dict):
        return
    mergelist = resume.get("mergelist")
    task = ["ebuild", eroot, cpv, "merge"]
    if not isinstance(mergelist, list) or task not in mergelist:
        return
    mergelist.remove(task)
    if not mergelist:
        del mtimedb["resume"]
