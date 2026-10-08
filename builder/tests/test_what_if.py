"""egraph --use and --env: the USE every candidate gets with lines tried, held to portage's with
the lines saved where egraph would save them."""

import os

import pytest
from conftest import System, portdb, write_stores
from scenarios import SCENARIOS
from test_ledger import EGRAPH, egraph_use

from egraph_build import evaluated

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def save(playground, lines):
    """Appends lines, (file, text) pairs, to package.use/egraph or package.env/egraph (to the
    file itself where package.use or package.env is one), flags alone after */*."""
    user = os.path.join(playground.eroot, "etc", "portage")
    for name, text in lines:
        path = os.path.join(user, f"package.{name}")
        if not os.path.isfile(path):
            os.makedirs(path, exist_ok=True)
            path = os.path.join(path, "egraph")
        if "/" not in text.split()[0]:
            text = "*/* " + text
        with open(path, "a", encoding="utf-8") as out:
            out.write(text + "\n")


def candidates_use(system):
    """{cpv::repo: (USE, forced)} for every candidate, as the builder evaluates them."""
    layer, _ = evaluated.rebuild(system.vardb, portdb(system), None, None)
    return {
        f"{c.cpv}::{c.repo}": (sorted(c.use), sorted(c.forced))
        for c in layer.candidates()
    }


def tried_and_saved(playground, tmp_path, lines):
    """{cpv::repo: (USE, forced)} before, as egraph answers with lines tried, and as portage
    evaluates them once saved."""
    trees = playground.trees
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    before = candidates_use(system)
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    options = ["--config-root", playground.eroot]
    for name, text in lines:
        options += [f"--{name}", text]
    found = egraph_use(path, options=options)
    tried = {
        pkg: (
            sorted(flag for flag, (on, _, _) in flags.items() if on),
            sorted(flag for flag, (_, fixed, _) in flags.items() if fixed),
        )
        for pkg, flags in found.items()
    }
    save(playground, lines)
    _, changed = playground._load_config()
    saved = System(
        playground.eroot, changed[playground.eroot]["vartree"].dbapi, changed
    )
    after = candidates_use(saved)
    # egraph use lists nothing for a candidate without flags.
    return before, {pkg: tried.get(pkg, ([], [])) for pkg in after}, after


def toggles(system):
    """A line per installed cp with ebuilds turning its first flag the other way, and one for
    every package doing that to the first flag of all."""
    layer, _ = evaluated.rebuild(system.vardb, portdb(system), None, None)
    installed = set(system.vardb.cp_all())
    lines, seen = [], set()
    for c in layer.candidates():
        flags = sorted(set(c.iuse) - set(c.forced))
        if c.cp not in installed or c.cp in seen or not flags:
            continue
        seen.add(c.cp)
        flag = flags[0]
        lines.append(("use", f"{c.cp} {'-' if flag in c.use else ''}{flag}"))
    if lines:
        first = lines[0][1].split()[1]
        lines.append(("use", first[1:] if first.startswith("-") else "-" + first))
    return lines


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_flags_tried_are_portages(name, mutable_playground, tmp_path):
    playground = mutable_playground(name)
    trees = playground.trees
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    lines = toggles(system)
    if not lines:
        pytest.skip("no installed package has a flag to toggle")
    before, tried, saved = tried_and_saved(playground, tmp_path, lines)
    assert tried == saved
    assert tried != before


def test_the_ledger_scenarios_lines_tried_are_portages(mutable_playground, tmp_path):
    """USE_EXPAND prefixes, a */* line among those of files sorting before and after egraph's,
    an atom more specific than the line's, and env files named already, and not yet, for a
    package and for every one."""
    playground = mutable_playground("ledger")
    lines = [
        ("use", "app-misc/a VIDEO_CARDS: intel -nvidia"),
        ("use", "app-misc/a -ranged exact"),
        ("use", "-glob LINGUAS: de"),
        ("use", "app-misc/c -idefault"),
        ("env", "app-misc/a tried/video.conf"),
        ("env", "app-misc/b withenv.conf"),
        ("env", "*/* tried/video.conf"),
    ]
    before, tried, saved = tried_and_saved(playground, tmp_path, lines)
    assert tried == saved
    changed = {pkg for pkg in tried if tried[pkg] != before[pkg]}
    assert {pkg.split("::")[0] for pkg in changed} >= {"app-misc/a-1", "app-misc/c-1"}
