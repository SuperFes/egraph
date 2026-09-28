"""egraph refreshing its store through egraph-build as the system changes."""

import os
import shutil
import subprocess
import sys

import portage
import pytest

from egraph_build import build, installed
from test_build import add_package, add_to_world, age, fresh_vardb, vdb

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

BUILDER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTAGE_LIB = os.path.dirname(os.path.dirname(portage.__file__))


@pytest.fixture
def system(mutable_playground, tmp_path):
    playground = mutable_playground("reference")
    age(playground.eroot)
    log = tmp_path / "builds"
    builder = tmp_path / "egraph-build"
    builder.write_text(
        "#!/bin/sh\n"
        f'echo "$@" >> "{log}"\n'
        f'PYTHONPATH="{BUILDER_DIR}:{PORTAGE_LIB}" exec "{sys.executable}" -m egraph_build "$@"\n'
    )
    builder.chmod(0o755)
    return playground, tmp_path / "installed.egraph", builder, log


def egraph(system, *args):
    playground, store, builder, _ = system
    return subprocess.run(
        [
            EGRAPH,
            "--store",
            str(store),
            "--config-root",
            playground.eroot,
            "--eprefix",
            playground.eprefix,
            "--builder",
            str(builder),
            *args,
        ],
        capture_output=True,
        text=True,
        env=dict(os.environ, EGRAPH_STRICT="1"),
    )


def query(system, *extra):
    return egraph(system, *extra, "export", "--format", "json")


def expected(playground):
    return installed.to_json(build.full(fresh_vardb(playground)).layer)


def builds(system):
    log = system[3]
    return log.read_text().splitlines() if log.exists() else []


def test_first_query_builds_the_store(system):
    playground = system[0]
    result = query(system)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected(playground)
    assert len(builds(system)) == 1


def test_fresh_store_is_not_rebuilt(system):
    query(system)
    result = query(system)
    assert result.returncode == 0, result.stderr
    assert len(builds(system)) == 1


@pytest.mark.parametrize(
    "change",
    [
        lambda p: add_package(p, "dev-libs/alt-b-1"),
        lambda p: shutil.rmtree(vdb(p, "dev-libs/cond-1")),
        lambda p: add_package(p, "sys-apps/new-1", RDEPEND="dev-libs/lib:2"),
        lambda p: add_to_world(p, "app-misc/old"),
    ],
    ids=["added", "removed", "new-category", "world"],
)
def test_changes_are_picked_up(system, change):
    playground = system[0]
    query(system)
    change(playground)
    result = query(system)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected(playground)
    assert len(builds(system)) == 2
    assert builds(system)[-1].startswith("--incremental --store ")


def test_no_refresh_answers_from_the_stale_store(system):
    playground = system[0]
    before = query(system).stdout
    add_package(playground, "dev-libs/alt-b-1")
    result = query(system, "--no-refresh")
    assert result.returncode == 0
    assert result.stdout == before
    assert "warning: answering from a stale store" in result.stderr
    assert len(builds(system)) == 1


def test_rebuild_writes_a_full_store(system):
    result = egraph(system, "rebuild")
    assert result.returncode == 0, result.stderr
    assert builds(system)[-1].startswith("--full --store ")
    assert query(system).stdout == expected(system[0])


def test_check_finds_no_drift_in_a_current_store(system):
    query(system)
    result = egraph(system, "check")
    assert (result.returncode, result.stdout, result.stderr) == (0, "", "")


def test_check_reports_what_the_store_missed(system):
    playground = system[0]
    query(system)
    add_package(playground, "dev-libs/alt-b-1")
    shutil.rmtree(vdb(playground, "dev-libs/nocond-1"))
    result = egraph(system, "check")
    assert result.returncode == 4
    # alt-b-1 is new, nocond-1 is gone, and root-1's || now matches alt-b-1.
    assert result.stdout.splitlines() == [
        "~app-misc/root-1",
        "+dev-libs/alt-b-1",
        "-dev-libs/nocond-1",
    ]


def test_refresh_brings_the_store_up_to_date_and_prints_nothing(system):
    playground = system[0]
    result = egraph(system, "refresh")
    assert (result.returncode, result.stdout) == (0, "")
    assert len(builds(system)) == 1
    # Current: nothing to do.
    assert egraph(system, "refresh").returncode == 0
    assert len(builds(system)) == 1
    add_package(playground, "dev-libs/alt-b-1")
    assert egraph(system, "refresh").returncode == 0
    assert builds(system)[-1].startswith("--incremental --store ")
    assert query(system, "--no-refresh").stdout == expected(playground)
