"""egraph config check on every scenario: its findings, and its exit status."""

import os
import subprocess

import pytest
from conftest import write_index, write_stores

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

SEVERITIES = {
    "dead": "error",
    "contradicted": "error",
    "no-effect": "warning",
    "not-installed": "note",
}


def check(system, tmp_path, *options):
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    write_index(system, path)
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", *options, "config", "check"],
        capture_output=True,
        text=True,
    )


def test_findings_are_records_and_errors_fail_the_check(scenario, tmp_path):
    result = check(scenario, tmp_path, "--layout", "lines")
    records = [line.split("\t") for line in result.stdout.splitlines()]
    assert all(len(fields) == 7 for fields in records), result.stdout
    assert all(SEVERITIES[fields[3]] == fields[2] for fields in records)
    errors = any(fields[2] == "error" for fields in records)
    assert result.returncode == (1 if errors else 0), result.stderr


def test_a_clean_configuration_says_so(playgrounds, tmp_path):
    result = check(playgrounds("reference"), tmp_path, "--layout", "human")
    assert (result.returncode, result.stdout) == (0, "no findings\n"), result.stderr
