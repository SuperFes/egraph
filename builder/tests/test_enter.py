"""egraph enter, as a run in a snapshot starts its workers: a mount namespace of its own (in a
user namespace of its own when not root), the binds, and a chroot or the snapshot bound over a
prefix."""

import os
import subprocess

import pytest

EGRAPH = os.environ.get("EGRAPH")


def can_enter():
    """Root, or a user the kernel lets have a user namespace."""
    if os.geteuid() == 0:
        return True
    try:
        return subprocess.run(["unshare", "--user", "true"]).returncode == 0
    except OSError:
        return False


pytestmark = [
    pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary"),
    pytest.mark.skipif(not can_enter(), reason="needs root or user namespaces"),
]


def confined_root(path):
    """A root holding nothing but the host's top-level links to /usr (merged or not)."""
    path.mkdir()
    for name in ("bin", "lib", "lib64", "sbin"):
        host = os.path.join("/", name)
        if os.path.islink(host):
            (path / name).symlink_to(os.readlink(host))
    return path


def enter(snapshot, binds, command, target=None):
    argv = [EGRAPH, "enter"]
    for path, read_only in binds:
        argv += ["--read-only" if read_only else "--bind", str(path)]
    if target is not None:
        argv += ["--target", str(target)]
    return subprocess.run(
        argv + [str(snapshot), "--"] + command, capture_output=True, text=True
    )


def test_a_command_runs_inside_with_only_the_binds(tmp_path):
    root = confined_root(tmp_path / "root")
    shared = tmp_path / "shared"
    shared.mkdir()
    script = (
        "test -r /proc/self/status && test ! -e /etc/portage"
        f" && ! touch /usr/egraph-enter 2>/dev/null && echo inside > {shared}/out"
    )
    ran = enter(
        root,
        [("/usr", True), ("/proc", False), (shared, False)],
        ["/bin/sh", "-c", script],
    )
    assert ran.returncode == 0, ran.stderr
    assert (shared / "out").read_text() == "inside\n"
    # The mounts went with the namespace.
    assert not any((root / "usr").iterdir())
    assert not (root / "usr" / "egraph-enter").exists()


def test_a_command_that_cannot_run_inside_fails_saying_so(tmp_path):
    root = confined_root(tmp_path / "root")
    ran = enter(root, [("/usr", True)], ["/no/such/program"])
    assert ran.returncode != 0
    assert "egraph: enter: /no/such/program:" in ran.stderr


def test_a_bind_that_is_not_there_fails_before_the_command(tmp_path):
    root = confined_root(tmp_path / "root")
    ran = enter(root, [(tmp_path / "missing", False)], ["/bin/true"])
    assert ran.returncode != 0
    assert f"binding {tmp_path / 'missing'}:" in ran.stderr


def test_a_snapshot_stands_in_for_a_root_other_than_slash(tmp_path):
    """Under a prefix, the snapshot is bound over it, the binds inside it kept real, and the
    rest of the system left as it is."""
    target = tmp_path / "prefix"
    (target / "var" / "tmp").mkdir(parents=True)
    (target / "real").write_text("real\n")
    snapshot = tmp_path / "snapshot"
    snapshot.mkdir()
    (snapshot / "copy").write_text("copy\n")
    script = (
        f"test -e {target}/copy && test ! -e {target}/real && test -x /bin/sh"
        f" && echo inside > {target}/var/tmp/out && echo copied > {target}/copy"
    )
    ran = enter(
        snapshot,
        [(target / "var" / "tmp", False), ("/proc", False)],
        ["/bin/sh", "-c", script],
        target=target,
    )
    assert ran.returncode == 0, ran.stderr
    assert (target / "var" / "tmp" / "out").read_text() == "inside\n"
    assert (snapshot / "copy").read_text() == "copied\n"
    assert not (target / "copy").exists()
    assert (target / "real").read_text() == "real\n"
