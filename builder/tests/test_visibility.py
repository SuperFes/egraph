"""egraph's visibility of every version in the repositories against portage's: portdb's
match-visible less what depgraph finds invalid, and the reasons as depgraph's get_masking_status
words them (getmaskingstatus's, then each invalid string), on every scenario."""

import os
import subprocess

import portage
import pytest
from conftest import portdb
from portage.dep import Atom
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


def egraph_view(system, tmp_path):
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
    found = {}
    for line in result.stdout.splitlines():
        fields = line.split("\t")
        found[fields[0]] = (fields[2] == "visible", tuple(fields[3:]))
    return found


def test_every_version_is_visible_or_masked_as_portage_has_it(scenario, tmp_path):
    assert egraph_view(scenario, tmp_path) == portage_view(portdb(scenario))


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
