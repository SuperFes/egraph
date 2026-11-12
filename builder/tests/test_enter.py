"""egraph enter, as a run in a snapshot starts its workers: a mount namespace of its own, the
binds, and a chroot. Root only; run by hand under sudo before a release."""

import os
import subprocess

import pytest

EGRAPH = os.environ.get("EGRAPH")

pytestmark = [
    pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary"),
    pytest.mark.skipif(os.geteuid() != 0, reason="needs root"),
]


def confined_root(path):
    """A root holding nothing but the host's top-level links to /usr (merged or not)."""
    path.mkdir()
    for name in ("bin", "lib", "lib64", "sbin"):
        host = os.path.join("/", name)
        if os.path.islink(host):
            (path / name).symlink_to(os.readlink(host))
    return path


def enter(root, binds, command):
    argv = [EGRAPH, "enter"]
    for path, read_only in binds:
        argv += ["--read-only" if read_only else "--bind", str(path)]
    return subprocess.run(
        argv + [str(root), "--"] + command, capture_output=True, text=True
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
