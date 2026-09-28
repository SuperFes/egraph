"""What each package on a merge list waits for, from the ebuilds and binary packages."""

import json

import pytest

from egraph_build import cli, pending
from egraph_build.pending import Entry

EBUILDS = {
    # Built with feature on (IUSE default), so it waits for opt; its any-of group names two
    # pending packages and counts both. It is not held back by what it blocks, nor by PDEPEND.
    "app-misc/top-1": {
        "EAPI": "8",
        "IUSE": "+feature",
        "DEPEND": "dev-libs/lib",
        "RDEPEND": "feature? ( dev-libs/opt ) !dev-libs/blocked",
        "PDEPEND": "dev-libs/post",
        "BDEPEND": "|| ( dev-util/tool dev-util/other )",
    },
    # feature is off here.
    "app-misc/off-1": {
        "EAPI": "8",
        "IUSE": "feature",
        "RDEPEND": "feature? ( dev-libs/opt )",
    },
    "dev-libs/lib-1": {"EAPI": "8", "SLOT": "1"},
    "dev-libs/lib-2": {"EAPI": "8", "SLOT": "2/2.1"},
    "dev-libs/opt-1": {"EAPI": "8"},
    "dev-libs/post-1": {"EAPI": "8", "RDEPEND": "app-misc/top"},
    "dev-libs/blocked-1": {"EAPI": "8"},
    "dev-util/tool-2": {"EAPI": "8"},
    "dev-util/other-1": {"EAPI": "8"},
    # Only slot 1 of lib, which is not pending.
    "dev-libs/old-user-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:1"},
}

# Built with x, which its binary package records.
BINPKGS = {
    "dev-libs/bin-1": {
        "EAPI": "8",
        "IUSE": "x",
        "USE": "x",
        "RDEPEND": "x? ( dev-libs/lib:2= )",
    },
}

ENTRIES = [Entry("ebuild", cpv) for cpv in EBUILDS if cpv != "dev-libs/lib-1"] + [
    Entry("binary", "dev-libs/bin-1")
]

EXPECTED = {
    "app-misc/off-1": [],
    "app-misc/top-1": [
        "dev-libs/lib-2",
        "dev-libs/opt-1",
        "dev-util/other-1",
        "dev-util/tool-2",
    ],
    "dev-libs/bin-1": ["dev-libs/lib-2"],
    "dev-libs/blocked-1": [],
    "dev-libs/lib-2": [],
    "dev-libs/old-user-1": [],
    "dev-libs/opt-1": [],
    "dev-libs/post-1": ["app-misc/top-1"],
    "dev-util/other-1": [],
    "dev-util/tool-2": [],
}


@pytest.fixture(scope="module")
def system(gnupg_home):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(ebuilds=EBUILDS, binpkgs=BINPKGS)
    yield playground
    playground.cleanup()


def test_waits_follow_the_dependencies_a_merge_needs_first(system):
    tree = system.trees[system.eroot]
    result = pending.waits(
        system.settings, tree["porttree"].dbapi, tree["bintree"].dbapi, ENTRIES
    )
    assert result == EXPECTED


def test_a_package_whose_ebuild_is_gone_waits_for_nothing(system):
    tree = system.trees[system.eroot]
    entries = [Entry("ebuild", "dev-libs/gone-1"), Entry("ebuild", "dev-libs/post-1")]
    entries.append(Entry("ebuild", "app-misc/top-1"))
    result = pending.waits(
        system.settings, tree["porttree"].dbapi, tree["bintree"].dbapi, entries
    )
    assert result["dev-libs/gone-1"] == []
    assert result["dev-libs/post-1"] == ["app-misc/top-1"]


def test_cli_writes_them_to_a_file(system, tmp_path):
    output = tmp_path / "pending.json"
    argv = [
        "--pending",
        "--output",
        str(output),
        "--config-root",
        system.eroot,
        "--eprefix",
        system.eprefix,
        *(f"{entry.kind}:{entry.cpv}" for entry in ENTRIES),
    ]
    assert cli.main(argv) == cli.EXIT_OK
    assert json.loads(output.read_text()) == EXPECTED


@pytest.mark.parametrize(
    "argv",
    [
        ["--pending", "ebuild:x/y-1"],
        ["--pending", "--output", "/dev/null", "installed:x/y-1"],
        ["--full", "ebuild:x/y-1"],
    ],
)
def test_cli_rejects_what_it_cannot_do(argv, capsys):
    assert cli.main(argv) == cli.EXIT_USAGE
