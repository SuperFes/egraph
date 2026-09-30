"""The man pages: the builder's documents every option, and both are well-formed roff."""

import shutil
import subprocess
from pathlib import Path

import pytest

from egraph_build import cli

DOCS = Path(__file__).resolve().parents[2] / "docs"


def test_the_builder_page_documents_every_option():
    page = (DOCS / "egraph-build.1").read_text()
    for action in cli.parser()._actions:
        for name in action.option_strings:
            assert name.replace("-", "\\-") in page, name


@pytest.mark.parametrize("page", ["egraph.1", "egraph-build.1"])
def test_the_pages_lint_clean(page):
    if not shutil.which("mandoc"):
        pytest.skip("needs mandoc")
    result = subprocess.run(
        ["mandoc", "-Tlint", "-W", "warning", DOCS / page],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.stdout + result.stderr == ""
