"""What needs the user once emerge has run: configuration files with updates waiting, and
unread news. Both as emerge reports them after a merge, read without changing anything.
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


def to_json(config, news):
    return json.dumps(
        {
            "config": [{"file": file, "update": update} for file, update in config],
            "news": [
                {"repo": repo, "item": item, "title": title}
                for repo, item, title in news
            ],
        },
        indent=1,
        sort_keys=True,
    )
