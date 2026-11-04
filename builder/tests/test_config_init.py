"""egraph config example and init: the settings file as the dist file has it."""

import os
import subprocess
from pathlib import Path

EGRAPH = os.environ.get("EGRAPH", "build/egraph")


def run(root, *args):
    return subprocess.run(
        [EGRAPH, "--config-root", str(root), "config", *args],
        capture_output=True,
        text=True,
        check=False,
    )


def test_example_is_the_dist_file():
    dist = Path(__file__).parents[2] / "docs" / "egraph.conf.dist"
    assert run("/", "example").stdout == dist.read_text()


def test_init_writes_the_example_and_names_it(tmp_path):
    result = run(tmp_path, "init")
    path = tmp_path / "etc" / "egraph" / "egraph.conf"
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == str(path)
    assert path.read_text() == run("/", "example").stdout


def test_init_keeps_a_file_that_is_there_unless_forced(tmp_path):
    path = tmp_path / "etc" / "egraph" / "egraph.conf"
    path.parent.mkdir(parents=True)
    path.write_text("history_days = 30\n")
    refused = run(tmp_path, "init")
    assert refused.returncode == 1
    assert "--force" in refused.stderr
    assert path.read_text() == "history_days = 30\n"
    assert run(tmp_path, "init", "--force").returncode == 0
    assert path.read_text() == run("/", "example").stdout
