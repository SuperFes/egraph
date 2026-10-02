"""egraph exec: a plan carried out by egraph-build --worker leaves the system egraph install, with
the real emerge, leaves."""

import os
import subprocess

import pytest

from test_actions import EGRAPH, System
from test_build import age
from test_worker import EBUILDS, Machine, differences

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


@pytest.fixture
def machines(gnupg_home, tmp_path):
    """A playground with the worker's ebuilds, as an egraph System and a Machine over it."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(ebuilds=EBUILDS)
    system = System(playground, tmp_path)
    machine = Machine(playground, tmp_path)
    yield system, machine
    playground.cleanup()


def forget_stores(system):
    for path in (system.store, system.store.with_suffix(".evaluated.egraph")):
        if path.exists():
            path.unlink()


def carried_out_as_install(system, machine, before, args, command=None):
    """After the cpvs before, merged by emerge -1: egraph install and egraph exec, given args and
    --yes, leave the same system. exec's run."""
    for cpv in before:
        machine.emerge(f"-1 ={cpv}")
    age(system.playground.eroot)
    machine.save()
    installed = system.egraph("install", "--yes", *args)
    assert installed.returncode == 0, installed.stdout + installed.stderr
    emerged = machine.state()
    machine.restore()
    forget_stores(system)
    age(system.playground.eroot)
    ran = system.egraph(*(command or ["exec"]), "--yes", *args)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    worked = machine.state()
    assert worked == emerged, differences(emerged, worked)
    return ran


@pytest.mark.parametrize(
    "before, args, facts",
    [
        ([], ["app-misc/needs"], {"var/lib/portage/world": "app-misc/needs\n"}),
        (
            [],
            ["-1", "app-misc/needs"],
            {"var/lib/needs/postinst": "needs-1 after nothing\n"},
        ),
        (
            ["app-misc/files-1"],
            ["-u", "app-misc/files"],
            {"usr/share/files/files.conf": None},
        ),
        (
            ["app-misc/old-1"],
            ["app-misc/new"],
            {"usr/share/shared/data": "new\n", "var/db/pkg/app-misc/old-1": None},
        ),
    ],
    ids=["pulled-in", "oneshot", "replacing", "blocker"],
)
def test_exec_merges_as_install_does(machines, before, args, facts):
    """The image, vdb entries and world file: a package and the dependency it pulls in, the
    same without joining the world file, an update replacing an installed version, and a
    package whose blocker uninstalls one it takes a file over from."""
    system, machine = machines
    ran = carried_out_as_install(system, machine, before, args)
    for path, content in facts.items():
        if content is None:
            assert not os.path.lexists(machine.path(path)), path
        else:
            with open(machine.path(path)) as f:
                assert f.read() == content, path
    # A line per event, each request ending done.
    assert ran.stdout.count("\tmerged") + ran.stdout.count("\tuninstalled") >= 1


def test_egraph_exec_is_exec(machines, tmp_path):
    """Through a link named egraph-exec, the global options among the command's own."""
    system, machine = machines
    link = tmp_path / "egraph-exec"
    link.symlink_to(os.path.abspath(EGRAPH))
    age(system.playground.eroot)
    command = system.command("--yes", "app-misc/lib")
    ran = subprocess.run(
        [str(link), *command[1:]],
        capture_output=True,
        text=True,
        env=dict(os.environ, EGRAPH_STRICT="1"),
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert machine.installed("app-misc/lib-1")


def test_a_failed_build_stops_the_run(machines):
    """The step that failed is named with its phase and log; nothing after it runs."""
    system, machine = machines
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "app-misc/broken", "app-misc/lib")
    assert ran.returncode != 0
    assert "app-misc/broken-1: compile failed with status 1 (log: " in ran.stderr
    assert not machine.installed("app-misc/broken-1")
    first = ran.stdout.splitlines()[0].split("\t")
    if first[1] == "app-misc/broken-1":
        assert not machine.installed("app-misc/lib-1")
