"""egraph search against emerge's own search (_emerge.search, test code only): the packages each
key finds, and the version, installed version, homepage, license and description shown.
"""

import os
import subprocess

import pytest
from conftest import portdb, write_stores

from egraph_build import repository, store

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def emerge_lines(system, keys, searchdesc=False):
    """search_lines' lines, as emerge --search's output has them."""
    import portage
    from _emerge.search import search

    root_config = system.trees[system.eroot]["root_config"]
    lines = []
    for key in keys:
        # As action_search makes it, with emerge's defaults.
        found = search(
            root_config,
            searchdesc,
            True,
            False,
            False,
            search_index=True,
            fuzzy=True,
            regex_auto=True,
        )
        found.execute(key)
        for kind, cp in found._iter_search():
            if kind == "set":
                continue
            full = found._xmatch("bestmatch-visible", cp)
            masked = not full
            if masked:
                full = (found._xmatch("match-all", cp) or [""])[-1]
            homepage = license_ = description = ""
            if full and found._portdb.cpv_exists(full):
                homepage, license_, description = found._aux_get(
                    full, ["HOMEPAGE", "LICENSE", "DESCRIPTION"]
                )
            installed = found._vardb.match(cp)
            lines.append(
                "\t".join(
                    (
                        key,
                        cp,
                        found.getVersion(full, search.VERSION_RELEASE),
                        "masked" if masked else "visible",
                        found.getVersion(
                            portage.best(installed) if installed else "",
                            search.VERSION_RELEASE,
                        ),
                        homepage,
                        " ".join(license_.split()),
                        description,
                    )
                )
            )
    return lines


def egraph_lines(system, tmp_path, keys, *options):
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    meta = store.RepositoryMeta("0", "0", "/", 0)
    index = repository.read(portdb(system))
    store.write(store.repository_path(path), store.encode_repository(index, meta))
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "search", *options, "--", *keys],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.splitlines()


def keys_for(system):
    """Each cp's name, its first letters, a misspelling, its category, and a regex."""
    cps = sorted(set(portdb(system).cp_all()) | set(system.vardb.cp_all()))
    keys = set()
    for cp in cps:
        category, name = cp.split("/")
        keys.update((name, name[:3], category + "/"))
        if len(name) > 3:
            keys.add(name[0] + name[2] + name[1] + name[3:])
    keys.update(("%^dev", "lib$", "@app", "zzz"))
    return sorted(keys)


def test_search_finds_and_shows_what_emerge_does(scenario, tmp_path):
    keys = keys_for(scenario)
    assert egraph_lines(scenario, tmp_path, keys) == emerge_lines(scenario, keys)


def test_search_matches_descriptions_with_searchdesc(playgrounds, tmp_path):
    system = playgrounds("visibility")
    keys = ["toolkit", "TESTING", "%^a steady"]
    ours = egraph_lines(system, tmp_path, keys, "-S")
    assert ours == emerge_lines(system, keys, searchdesc=True)
    assert [line.split("\t")[1] for line in ours] == [
        "app-misc/stable",
        "app-misc/testing",
        "app-misc/testing",
        "app-misc/stable",
    ]


def test_without_fuzzy_search_only_the_text_matches(playgrounds, tmp_path):
    system = playgrounds("visibility")
    assert egraph_lines(system, tmp_path, ["stalbe"])
    assert egraph_lines(system, tmp_path, ["stalbe"], "--fuzzy-search", "n") == []
