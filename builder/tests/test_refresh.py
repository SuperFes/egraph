"""egraph refreshing its store through egraph-build as the system changes."""

import os
import shutil
import subprocess
import sys

import portage
import pytest

from egraph_build import build, installed
from test_build import add_package, age, fresh_vardb, vdb

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


def query(system, *extra, strict=True):
    playground, store, builder, _ = system
    env = dict(os.environ, EGRAPH_STRICT="1" if strict else "0")
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
            *extra,
            "export",
            "--format",
            "json",
        ],
        capture_output=True,
        text=True,
        env=env,
    )


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
    ],
    ids=["added", "removed", "new-category"],
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
