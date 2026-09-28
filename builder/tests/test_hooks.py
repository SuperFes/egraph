"""The portage hooks egraph installs: the post_emerge dispatcher and egraph's entry in it."""

import os
import re
import stat
import subprocess
from pathlib import Path

import pytest

from test_build import age

SOURCE = Path(__file__).resolve().parents[2]
DISPATCHER = SOURCE / "hooks" / "post_emerge"
EGRAPH = os.environ.get("EGRAPH")


def executable(path, text):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def run_dispatcher(config_root, **env):
    return subprocess.run(
        [str(DISPATCHER)],
        env=dict(os.environ, PORTAGE_CONFIGROOT=str(config_root), **env),
        capture_output=True,
        text=True,
    )


def test_dispatcher_runs_every_hook_in_name_order(tmp_path):
    hooks = tmp_path / "etc" / "portage" / "post_emerge.d"
    hooks.mkdir(parents=True)
    log = tmp_path / "log"
    for name in ("20-second", "10-first"):
        executable(hooks / name, f'#!/bin/sh\necho "{name} $ROOT" >> "{log}"\n')
    # Neither a file nor executable: not a hook.
    (hooks / "30-notes").write_text("#!/bin/sh\nexit 1\n")
    (hooks / "40-dir").mkdir()
    result = run_dispatcher(tmp_path, ROOT="/mnt/target")
    assert result.returncode == 0, result.stderr
    assert log.read_text() == "10-first /mnt/target\n20-second /mnt/target\n"


def test_one_failing_hook_does_not_stop_the_rest(tmp_path):
    hooks = tmp_path / "etc" / "portage" / "post_emerge.d"
    hooks.mkdir(parents=True)
    executable(hooks / "10-fails", "#!/bin/sh\nexit 3\n")
    executable(hooks / "20-runs", f'#!/bin/sh\ntouch "{tmp_path / "ran"}"\n')
    result = run_dispatcher(tmp_path)
    assert result.returncode == 1
    assert "10-fails failed" in result.stderr
    assert (tmp_path / "ran").exists()


def test_no_hooks_is_nothing_to_do(tmp_path):
    result = run_dispatcher(tmp_path)
    assert (result.returncode, result.stdout, result.stderr) == (0, "", "")


@pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary")
def test_egraph_hook_refreshes_the_store_after_an_emerge(mutable_playground):
    # meson configures the hook next to the binary it runs.
    hook = Path(EGRAPH).parent / "post_emerge-egraph"
    playground = mutable_playground("reference")
    age(playground.eroot)
    hooks = Path(playground.eroot) / "etc" / "portage" / "post_emerge.d"
    hooks.mkdir(parents=True, exist_ok=True)
    # Installed as meson installs it: the build directory's copy is not executable.
    executable(hooks / "egraph", hook.read_text())
    store = Path(playground.eprefix) / "var" / "cache" / "egraph" / "installed.egraph"
    # As portage runs it: its roots in the environment, the binary where meson installs it.
    env = {"ROOT": "/", "EPREFIX": playground.eprefix, "EGRAPH": EGRAPH}
    result = run_dispatcher(playground.eroot, **env)
    assert result.returncode == 0, result.stderr
    assert store.exists()
    built = store.stat().st_mtime_ns
    # Current: left alone.
    assert run_dispatcher(playground.eroot, **env).returncode == 0
    assert store.stat().st_mtime_ns == built


def test_every_builder_module_is_installed():
    listed = set(
        re.findall(
            r"'builder/egraph_build/(\w+\.py)'", (SOURCE / "meson.build").read_text()
        )
    )
    modules = {path.name for path in (SOURCE / "builder" / "egraph_build").glob("*.py")}
    assert modules == listed
