"""What needs the user once emerge has run: configuration files with updates waiting, unread
news, and preserved libraries with what uses them. All as emerge reports them after a merge,
read without changing anything.
"""

import fnmatch
import json
import os

from portage.const import NEWS_LIB_PATH
from portage.util import find_updated_config_files, grabfile

# Where GLEP 42 puts a repository's news items, and the language emerge reads them in.
NEWS_PATH = os.path.join("metadata", "news")
LANGUAGE = "en"
# An update's name: the file's own after "._cfg" and four digits.
_PREFIX = len("._cfg0000_")


def _updates_of(path):
    """The pending updates of a single protected file, as emerge's find matches them."""
    head, tail = os.path.split(path.rstrip(os.sep))
    try:
        names = os.listdir(head)
    except OSError:
        return []
    return [
        os.path.join(head, name)
        for name in names
        if fnmatch.fnmatch(name, f"._cfg????_{tail}")
        and not name.endswith("~")
        and not name.lower().endswith(".bak")
    ]


def config_updates(settings):
    """(file, update) for each ._cfg file waiting under CONFIG_PROTECT, sorted."""
    protect = settings.get("CONFIG_PROTECT", "").split()
    found = set()
    for path, files in find_updated_config_files(settings["EROOT"], protect):
        for update in files if files is not None else _updates_of(path):
            head, tail = os.path.split(update)
            found.add((os.path.join(head, tail[_PREFIX:]), update))
    return sorted(found)


def _title(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line.strip():
                    break
                key, _, value = line.partition(":")
                if key == "Title":
                    return value.strip()
    except OSError:
        pass
    return ""


def unread_news(settings, portdb):
    """(repo, item, title) for each news item the repositories' unread lists hold, sorted.

    As emerge: none without FEATURES=news or without a profile to judge relevance by.
    """
    if "news" not in settings.features or not settings.profile_path:
        return []
    unread_path = os.path.join(settings["EROOT"], NEWS_LIB_PATH, "news")
    found = []
    for repo in portdb.getRepositories():
        location = portdb.getRepositoryPath(repo)
        for item in grabfile(os.path.join(unread_path, f"news-{repo}.unread")):
            text = os.path.join(location, NEWS_PATH, item, f"{item}.{LANGUAGE}.txt")
            found.append((repo, item, _title(text)))
    return sorted(found)


def preserved_libraries(vardb):
    """(path, package, consumers) for each library emerge preserved, consumers being the
    installed packages using it, and the atoms of @preserved-rebuild, as emerge loads the set.

    None for both when the registry cannot be read (only root and the portage group may), and
    None for the atoms and no consumers when the linkage map cannot be built. The registry, the
    linkage map and the set configuration are private to portage; only this function uses them.
    """
    from portage.exception import CommandNotFound, PermissionDenied

    from egraph_build.roots import _set_config

    try:
        libraries = vardb._plib_registry.getPreservedLibs()
    except PermissionDenied:
        return None, None
    if not libraries:
        return [], []
    linkmap = vardb._linkmap
    try:
        linkmap.rebuild()
    except CommandNotFound:
        return (
            sorted(
                (path, cpv, ()) for cpv, paths in libraries.items() for path in paths
            ),
            None,
        )
    preserved = {path for paths in libraries.values() for path in paths}
    found = []
    for cpv, paths in libraries.items():
        for path in paths:
            consumers = set(linkmap.findConsumers(path, greedy=False))
            # As emerge shows them: other preserved libraries only when nothing else uses it.
            using = (consumers - preserved) or consumers
            owners = {
                owner
                for consumer in using
                for owner in linkmap.getOwners(consumer)
                if vardb.cpv_exists(owner)
            }
            found.append((path, cpv, tuple(sorted(owners))))
    atoms = _set_config(vardb).getSetAtoms("preserved-rebuild")
    return sorted(found), sorted(str(atom) for atom in atoms)


def to_json(config, news, preserved=(), rebuild=()):
    return json.dumps(
        {
            "config": [{"file": file, "update": update} for file, update in config],
            "news": [
                {"repo": repo, "item": item, "title": title}
                for repo, item, title in news
            ],
            "preserved": (
                None
                if preserved is None
                else [
                    {"path": path, "package": cpv, "consumers": list(consumers)}
                    for path, cpv, consumers in preserved
                ]
            ),
            "rebuild": None if rebuild is None else list(rebuild),
        },
        indent=1,
        sort_keys=True,
    )
