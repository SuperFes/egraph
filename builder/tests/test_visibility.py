"""egraph's visibility of every version in the repositories against portage's: portdb's
match-visible less what depgraph finds invalid, and the reasons as depgraph's get_masking_status
words them (getmaskingstatus's, then each invalid string), on every scenario."""

import os
import re
import subprocess

import portage
import pytest
from conftest import portdb
from portage.dep import Atom, match_from_list
from portage.package.ebuild.getmaskingreason import getmaskingreason
from portage.package.ebuild.getmaskingstatus import getmaskingstatus

from egraph_build import installed, masks, repository, store

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def portage_view(db):
    """{cpv::repo: (visible, reasons)} as portage sees every version."""
    settings = portage.config(clone=db.settings)
    found = {}
    for cp in db.cp_all():
        for repo in db.getRepositories():
            listed = db.cp_list(cp, mytree=db.getRepositoryPath(repo))
            visible = set(db.xmatch("match-visible", Atom(f"{cp}::{repo}")))
            for cpv in listed:
                reasons = ()
                if cpv not in visible:
                    reasons = tuple(
                        getmaskingstatus(cpv, settings=settings, portdb=db, myrepo=repo)
                    )
                keys = list(masks.EBUILD_KEYS)
                metadata = dict(zip(keys, db.aux_get(cpv, keys, myrepo=repo)))
                metadata["repository"] = repo
                invalid = tuple(
                    f"invalid: {message}"
                    for message in masks.invalid_ebuild(db, cpv, metadata)
                )
                if not metadata["SLOT"]:
                    invalid += ("SLOT: undefined",)
                found[f"{cpv}::{repo}"] = (
                    cpv in visible and not invalid,
                    reasons + invalid,
                )
    return found


_SOURCED = re.compile(r"(.*) \((/[^()]*)\)")


def sourced(reason):
    """A reason as `versions` shows it: its text, and the file:line places after it."""
    found = _SOURCED.fullmatch(reason)
    return (found[1], tuple(found[2].split(", "))) if found else (reason, ())


def versions_view(lines):
    """{cpv::repo: (visible, reasons, places)} from `versions`' lines."""
    found = {}
    for line in lines:
        fields = line.split("\t")
        reasons = tuple(sourced(reason) for reason in fields[3:])
        found[fields[0]] = (
            fields[2] == "visible",
            tuple(text for text, _ in reasons),
            tuple(places for _, places in reasons),
        )
    return found


def egraph_lines(system, tmp_path):
    path = tmp_path / "installed.egraph"
    meta = store.Meta("0.0.0", "3.0.0", "/", 0)
    store.write(path, store.encode(installed.build(system.vardb), meta))
    index_meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 0)
    index = repository.read(portdb(system))
    store.write(store.repository_path(path), store.encode_repository(index, index_meta))
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "versions"],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.splitlines()


def egraph_view(system, tmp_path):
    view = versions_view(egraph_lines(system, tmp_path))
    return {key: (visible, reasons) for key, (visible, reasons, _) in view.items()}


def test_every_version_is_visible_or_masked_as_portage_has_it(scenario, tmp_path):
    assert egraph_view(scenario, tmp_path) == portage_view(portdb(scenario))


# Each kind of reason and what the line deciding it sets, or the file it is in.
_DECIDERS = (
    (" license(s)", "ACCEPT_LICENSE", "package.license"),
    (" properties", "ACCEPT_PROPERTIES", "package.properties"),
    (" in RESTRICT", "ACCEPT_RESTRICT", "package.accept_restrict"),
    (" keyword", "ACCEPT_KEYWORDS", None),
)


def _line(place):
    file, _, number = place.rpartition(":")
    with open(file, encoding="utf-8") as lines:
        return file, lines.read().splitlines()[int(number) - 1]


def test_each_reason_names_the_line_that_decides_it(scenario, request, tmp_path):
    db = portdb(scenario)
    settings = portage.config(clone=db.settings)
    checked = 0
    for key, (_, reasons, places) in versions_view(
        egraph_lines(scenario, tmp_path)
    ).items():
        cpv, _, repo = key.partition("::")
        for reason, where in zip(reasons, places):
            if reason == "package.mask":
                # getmaskingreason finds the same atom in the same file.
                _, location = getmaskingreason(
                    cpv, settings=settings, portdb=db, myrepo=repo, return_location=True
                )
                file, line = _line(where[0])
                assert (file, len(where)) == (location, 1)
                pkg = db._pkg_str(cpv, repo)
                assert match_from_list(Atom(line.strip(), allow_repo=True), [pkg])
                checked += 1
                continue
            for suffix, variable, package_file in _DECIDERS:
                if reason.endswith(suffix) and not reason.startswith("missing"):
                    for place in where:
                        file, line = _line(place)
                        assert variable in line or (
                            package_file and package_file in file
                        ), (key, reason, place)
                        checked += 1
    if request.node.callspec.params["scenario"] == "visibility":
        assert checked


def test_the_visibility_scenario_masks_by_every_rule(playgrounds, tmp_path):
    ours = egraph_view(playgrounds("visibility"), tmp_path)
    assert ours["app-misc/ranged-2::test_repo"] == (True, ())
    assert ours["app-misc/ranged-3::test_repo"] == (False, ("~x86 keyword",))
    assert ours["app-misc/conditional-1::test_repo"] == (
        False,
        ("( EULA ) license(s)",),
    )
    assert ours["app-misc/future-1::test_repo"] == (
        False,
        ("EAPI 99", "invalid: SLOT: invalid value: ''", "SLOT: undefined"),
    )
    assert ours["app-misc/repo-masked-1::test_repo"] == (False, ("package.mask",))
    assert ours["app-misc/over-1::overlay"] == (True, ())
