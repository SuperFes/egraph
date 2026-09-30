"""The build as a package builds it: what meson install puts where, and the tests run in
portage's environment."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

EGRAPH = os.environ.get("EGRAPH")

pytestmark = [
    pytest.mark.skipif(not EGRAPH, reason="set EGRAPH to the egraph binary"),
    pytest.mark.skipif(not shutil.which("meson"), reason="needs meson"),
]


def test_install_ships_the_hooks_completions_and_man_pages_but_no_user_file(tmp_path):
    build = Path(EGRAPH).parent
    subprocess.run(
        [
            "meson",
            "install",
            "-C",
            str(build),
            "--destdir",
            str(tmp_path),
            "--no-rebuild",
        ],
        capture_output=True,
        check=True,
    )
    installed = {
        str(path.relative_to(tmp_path))
        for path in tmp_path.rglob("*")
        if not path.is_dir()
    }

    def one(suffix):
        found = [path for path in installed if path.endswith(suffix)]
        assert len(found) == 1, (suffix, found)
        return tmp_path / found[0]

    for suffix in [
        "etc/portage/post_emerge.d/egraph",
        "etc/portage/postsync.d/egraph",
        "share/egraph/post_emerge",
    ]:
        assert os.access(one(suffix), os.X_OK), suffix
    for suffix in [
        "bin/egraph",
        "bin/egraph-build",
        "share/bash-completion/completions/egraph.bash",
        "share/zsh/site-functions/_egraph",
        "share/fish/vendor_completions.d/egraph.fish",
        "share/man/man1/egraph.1",
        "share/man/man1/egraph-build.1",
    ]:
        one(suffix)
    # portage runs only this one file after an emerge, and it belongs to the user.
    assert not any(path.endswith("etc/portage/bin/post_emerge") for path in installed)


def test_the_unit_tests_ignore_the_roots_portage_exports():
    build = Path(EGRAPH).parent
    result = subprocess.run(
        ["meson", "test", "-C", str(build), "--no-rebuild", "unit"],
        capture_output=True,
        text=True,
        env=dict(os.environ, PORTAGE_CONFIGROOT="/", ROOT="/", EGRAPH_LAYOUT="human"),
    )
    assert result.returncode == 0, result.stdout
