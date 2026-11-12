"""egraph exec in a snapshot, on a playground whose root is a btrfs subvolume: every merge
happens in the snapshot, the binary packages merge into the root itself once all built, and a
failed run leaves the root as it was. Its owner may take the snapshot, so this runs without root
wherever the kernel lets a user have a user namespace and the base directory
(EGRAPH_BTRFS_TMPDIR, else /var/tmp) is on btrfs."""

import glob
import os
import shutil
import subprocess
import tempfile

import pytest

from test_actions import EGRAPH, System
from test_build import age
from test_enter import can_enter
from test_worker import EBUILDS, INSTALL, PLAIN, Machine, differences

BASE = os.environ.get("EGRAPH_BTRFS_TMPDIR", "/var/tmp")


def btrfs(command):
    try:
        return subprocess.run(["btrfs", *command], capture_output=True).returncode == 0
    except OSError:
        return False


def can_snapshot():
    """Whether a subvolume made in BASE can be snapshotted by this user."""
    if not (EGRAPH and can_enter() and os.path.isdir(BASE)):
        return False
    base = tempfile.mkdtemp(dir=BASE)
    try:
        made = os.path.join(base, "made")
        return btrfs(["subvolume", "create", made]) and btrfs(
            ["subvolume", "snapshot", made, os.path.join(base, "copy")]
        )
    finally:
        shutil.rmtree(base, ignore_errors=True)


pytestmark = pytest.mark.skipif(
    not can_snapshot(), reason=f"needs user namespaces and btrfs subvolumes in {BASE}"
)


def as_subvolume(path):
    """The directory at path turned into a subvolume holding the same."""
    made = path + ".subvolume"
    assert btrfs(["subvolume", "create", made])
    subprocess.run(["cp", "-a", path + "/.", made], check=True)
    shutil.rmtree(path)
    os.rename(made, path)


def over(ebuilds, tmp_path, monkeypatch, base):
    """A playground with ebuilds in a subvolume under base, as an egraph System and a Machine."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    monkeypatch.setattr(tempfile, "tempdir", base)
    playground = ResolverPlayground(ebuilds=ebuilds)
    monkeypatch.setattr(tempfile, "tempdir", None)
    as_subvolume(playground.eprefix)
    try:
        yield System(playground, tmp_path), Machine(playground, tmp_path)
    finally:
        playground.cleanup()
        shutil.rmtree(base, ignore_errors=True)


@pytest.fixture
def machines(gnupg_home, tmp_path, monkeypatch):
    base = tempfile.mkdtemp(dir=BASE, prefix="egraph-snapshot-")
    yield from over(EBUILDS, tmp_path, monkeypatch, base)


@pytest.fixture
def flaky(gnupg_home, tmp_path, monkeypatch):
    """The worker's ebuilds and one needing lib that fails to build while a marker, outside the
    snapshot, is there: the System, the Machine and the marker."""
    base = tempfile.mkdtemp(dir=BASE, prefix="egraph-snapshot-")
    marker = os.path.join(base, "fail")
    ebuilds = dict(EBUILDS)
    ebuilds["app-misc/flaky-1"] = {
        **PLAIN,
        "DEPEND": "app-misc/lib",
        "RDEPEND": "app-misc/lib",
        "MISC_CONTENT": INSTALL
        + f'src_compile() {{ [[ -e {marker} ]] && die "marked to fail"; }}\n',
    }
    for system, machine in over(ebuilds, tmp_path, monkeypatch, base):
        yield system, machine, marker


def put_back(machine):
    """The saved state, back into the subvolume, which stays one."""
    for name in os.listdir(machine.eprefix):
        path = os.path.join(machine.eprefix, name)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
        else:
            os.unlink(path)
    shutil.copytree(
        machine.snapshot, machine.eprefix, symlinks=True, dirs_exist_ok=True
    )


def forget_stores(system):
    for path in (system.store, system.store.with_suffix(".evaluated.egraph")):
        if path.exists():
            path.unlink()


def leftovers(machine):
    return glob.glob(machine.path("var/lib/egraph/snapshot-*"))


# What a binary merge records that a merge from source does not, as emerge -K does too: the
# package's checksum and build id, its CONTENTS in another order, and the loop variable its
# setup phase leaves in the saved environment.
BINARY_ONLY = ("BINPKGMD5", "BUILD_ID")


def merged_state(machine):
    state = machine.state()
    vdb = {}
    for path, content in state["vdb"].items():
        name = os.path.basename(path)
        if name in BINARY_ONLY:
            continue
        if name == "CONTENTS":
            content = sorted(content)
        elif name == "environment.bz2":
            content = [line for line in content if line != "declare -- f"]
        vdb[path] = content
    state["vdb"] = vdb
    return state


def test_a_run_in_a_snapshot_leaves_what_a_plain_run_leaves(machines):
    """A package pulling in a dependency it builds against, and one whose blocker goes, files
    taken over: built and merged in the snapshot, then merged into the root in plan order.
    """
    system, machine = machines
    machine.emerge("-1 =app-misc/old-1")
    age(system.playground.eroot)
    machine.save()
    args = ["app-misc/needs", "app-misc/new"]
    ran = system.egraph("exec", "--yes", "--snapshot", *args)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "egraph: exec: building in a snapshot of " in ran.stdout
    assert "egraph: exec: merging what built into " in ran.stdout
    snapshotted = merged_state(machine)
    assert leftovers(machine) == []
    assert not os.path.exists(machine.path("var/lib/egraph/packages"))
    put_back(machine)
    forget_stores(system)
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "--no-snapshot", *args)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    plain = merged_state(machine)
    assert snapshotted == plain, differences(plain, snapshotted)


def test_a_failed_run_in_a_snapshot_leaves_the_root_as_it_was(machines):
    system, machine = machines
    before = machine.state()
    ran = system.egraph(
        "exec", "--yes", "--snapshot", "app-misc/lib", "app-misc/broken"
    )
    assert ran.returncode != 0
    assert "app-misc/broken-1: compile failed with status 1" in ran.stderr
    assert " is as it was" in ran.stdout
    after = machine.state()
    assert after == before, differences(before, after)
    assert leftovers(machine) == []


def test_a_resumed_run_in_a_snapshot_takes_what_the_last_one_built(flaky):
    system, machine, marker = flaky
    open(marker, "w").close()
    ran = system.egraph("exec", "--yes", "--snapshot", "app-misc/flaky")
    assert ran.returncode != 0
    assert "app-misc/flaky-1: compile failed" in ran.stderr
    assert not machine.installed("app-misc/lib-1")
    kept = machine.path("var/lib/egraph/packages/app-misc")
    assert glob.glob(os.path.join(kept, "**", "lib-1*"), recursive=True)
    os.unlink(marker)
    age(system.playground.eroot)
    ran = system.egraph("exec", "--yes", "--resume")
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "app-misc/lib-1\tbinpkg" in ran.stdout
    assert "app-misc/flaky-1\tcompile" in ran.stdout
    assert machine.installed("app-misc/lib-1")
    assert machine.installed("app-misc/flaky-1")
    assert leftovers(machine) == []
    assert not os.path.exists(machine.path("var/lib/egraph/packages"))
