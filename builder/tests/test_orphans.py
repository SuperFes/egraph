"""egraph orphans against emerge --depclean, through the real binary."""

import os
import subprocess

import pytest

from conftest import dynamic_option, write_stores
from depclean import depclean

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def orphans(path, *options):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "orphans", *options],
        capture_output=True,
        text=True,
    )


def cpvs_of(stdout):
    """The cpvs of orphans' lines, each before its size."""
    return tuple(line.split("\t")[0] for line in stdout.splitlines())


def unresolved(stderr):
    """(parent, atom) pairs from egraph's refusal message."""
    return frozenset(
        tuple(line.strip().split("\t")[::2])
        for line in stderr.splitlines()
        if line.startswith("  ")
    )


@pytest.fixture
def system(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    return scenario, path


@pytest.mark.parametrize("with_bdeps", [True, False], ids=["bdeps", "no-bdeps"])
def test_orphans_are_what_depclean_removes(system, with_bdeps, dynamic_deps):
    scenario, path = system
    expected = depclean(scenario.trees, scenario.eroot, with_bdeps, dynamic_deps)
    result = orphans(
        path, "--with-bdeps", "y" if with_bdeps else "n", *dynamic_option(dynamic_deps)
    )
    assert (result.returncode != 0) == (expected.returncode != 0), result.stderr
    if not expected.kept:
        # Refused before resolving anything: an empty @world.
        assert result.stdout == ""
        assert "the @world set is empty" in result.stderr
        return
    assert cpvs_of(result.stdout) == expected.orphans
    assert unresolved(result.stderr) == expected.unresolved


def exclusions(scenario, orphaned):
    """Exclusions of the orphans, as emerge --exclude takes them: the first one's cp, the last
    one's name with its slot, and every package in the middle one's category."""
    from portage.versions import catpkgsplit

    if not orphaned:
        return []
    first, middle, last = orphaned[0], orphaned[len(orphaned) // 2], orphaned[-1]
    category, name, _, _ = catpkgsplit(last)
    (slot,) = scenario.vardb.aux_get(last, ["SLOT"])
    return [
        ["/".join(catpkgsplit(first)[:2])],
        [f"{name}:{slot.split('/')[0]}"],
        [f"{catpkgsplit(middle)[0]}/*"],
    ]


def test_exclusions_are_depclean_excludes(system, dynamic_deps):
    """What --exclude keeps, as emerge --depclean --exclude keeps it."""
    scenario, path = system
    plain = depclean(scenario.trees, scenario.eroot, dynamic_deps=dynamic_deps)
    if not plain.kept:
        pytest.skip("depclean refuses here")
    for exclude in exclusions(scenario, sorted(plain.orphans)):
        expected = depclean(
            scenario.trees, scenario.eroot, dynamic_deps=dynamic_deps, exclude=exclude
        )
        result = orphans(
            path,
            *dynamic_option(dynamic_deps),
            *(f"--exclude={atom}" for atom in exclude),
        )
        assert (result.returncode != 0) == (expected.returncode != 0), result.stderr
        assert cpvs_of(result.stdout) == expected.orphans, exclude
        assert unresolved(result.stderr) == expected.unresolved, exclude


def test_exclusions_take_what_emerge_takes(playgrounds, tmp_path):
    """--exclude refuses exactly the atoms emerge's option does."""
    from portage.repository.config import _find_bad_atoms

    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("reference"), path)
    for text in (
        "app-misc/foo",
        "foo",
        "app-misc/*",
        "*/foo",
        "*/*",
        "app-misc/foo:2",
        "foo:2",
        "app-misc/foo:2/3",
        "app-*/foo",
        ">=app-misc/foo-1",
        "=app-misc/foo-1",
        "~app-misc/foo-1",
        "app-misc/foo-1",
        "app-misc/foo[bar]",
        "app-misc/foo::gentoo",
        "!app-misc/foo",
        "app-misc/",
        "/foo",
    ):
        result = orphans(path, f"--exclude={text}")
        assert (result.returncode == 2) == bool(_find_bad_atoms([text])), (
            text,
            result.stderr,
        )


def test_roots_scenario_by_hand(playgrounds, tmp_path):
    """What depclean removes there, worked out by hand."""
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("roots"), path)
    result = orphans(path)
    assert result.returncode == 0, result.stderr
    assert cpvs_of(result.stdout) == (
        "app-misc/orphan-1",
        # || ( alt-x alt-y ): neither kept yet, so the first.
        "dev-libs/alt-y-1",
        # A cycle nothing else needs.
        "dev-libs/cycle-a-1",
        "dev-libs/cycle-b-1",
        # || ( impl-a impl-b ) in virtual/v: impl-b is already kept.
        "dev-libs/impl-a-1",
        "dev-libs/orphan-dep-1",
        # The unslotted world atom keeps the highest slot only.
        "sys-kernel/sources-1",
    )


def test_dependencies_follow_the_ebuild_unless_told_otherwise(playgrounds, tmp_path):
    """The ebuild dropped a dependency without a revision bump: emerge's default
    --dynamic-deps=y lets it go, and --dynamic-deps=n keeps what the vdb recorded."""
    system = playgrounds("dynamic-deps")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    assert orphans(path).stdout == "dev-libs/dep-1\t\n"
    assert depclean(system.trees, system.eroot, dynamic_deps=True).orphans == (
        "dev-libs/dep-1",
    )
    assert orphans(path, "--dynamic-deps", "n").stdout == ""
    assert depclean(system.trees, system.eroot, dynamic_deps=False).orphans == ()


def test_atoms_of_one_cp_share_what_they_keep(playgrounds, tmp_path):
    """slotop-1's ebuild says dev-libs/lib:=, which alone would keep lib-2, and emerge appends
    the dev-libs/lib:1/1= it was built with; emerge's _minimize_children then lets both atoms
    keep lib-1 and nothing keeps lib-2."""
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("repository"), path)
    assert "dev-libs/lib-2" in cpvs_of(orphans(path).stdout)
