"""egraph --use and --env: the USE every candidate gets with lines tried, held to portage's with
the lines saved where egraph would save them."""

import os
import subprocess

import pytest
from conftest import System, portdb, write_stores
from scenarios import SCENARIOS
from test_ledger import EGRAPH, egraph_use
from test_queries import EXIT_REFUSED, egraph, merged, new_use, refuses
from test_refresh import evaluations, scenario_system

from egraph_build import evaluated

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def save(playground, path, lines):
    """Saves lines, (file, text) pairs, as egraph save writes them to the playground's
    configuration, from the stores at path."""
    options = ["--config-root", playground.eroot]
    for name, text in lines:
        options += [f"--{name}", text]
    result = egraph(path, *options, "save", check=False)
    assert result.returncode == 0, result.stderr


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
    save(playground, path, lines)
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


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_plans_with_flags_tried_are_emerges(name, mutable_playground, tmp_path):
    """egraph updates -D with each toggle tried plans what emerge -puDU @installed does once
    they are saved: what the toggles change is rebuilt, and pulls in what it needs."""
    import update

    playground = mutable_playground(name)
    trees = playground.trees
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    lines = toggles(system)
    if not lines:
        pytest.skip("no installed package has a flag to toggle")
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    options = ["--config-root", playground.eroot]
    for file, text in lines:
        options += [f"--{file}", text]
    result = egraph(path, *options, "updates", "-D", check=False)
    before = update.updates(trees, playground.eroot, changed_use=True, deep=True)
    save(playground, path, lines)
    _, changed = playground._load_config()
    expected = update.updates(changed, playground.eroot, changed_use=True, deep=True)
    if not expected.success and not refuses(expected):
        pytest.skip("emerge cannot resolve @installed here")
    assert result.returncode == (
        EXIT_REFUSED if refuses(expected) else 0
    ), result.stderr
    if expected.unsatisfied or expected.unmet or expected.use_changes:
        return
    assert merged(result.stdout) == (expected.replaced, expected.rebuilt, expected.new)
    assert new_use(result.stdout) == expected.use
    if not before.success or refuses(before):
        return
    assert tried_rows(result.stdout) == differences(before, expected)


def tried_rows(text):
    """{first field: change} from the tried rows of updates output."""
    rows = [line.split("\t") for line in text.splitlines()]
    return {
        fields[0]: fields[2]
        for fields in rows
        if fields[1] == "tried" and fields[2] != "none"
    }


def differences(before, after):
    """{installed or new cpv: change} between two of emerge's plans, as tried rows name them."""

    def merges(plan):
        found = dict(plan.replaced)
        found.update((cpv, "rebuild") for cpv in plan.rebuilt)
        found.update((cpv, ("new", plan.use.get(cpv, ""))) for cpv in plan.new)
        return found

    was, now = merges(before), merges(after)
    found = {cpv: "added" for cpv in now.keys() - was.keys()}
    found.update((cpv, "dropped") for cpv in was.keys() - now.keys())
    found.update(
        (cpv, "changed") for cpv in now.keys() & was.keys() if now[cpv] != was[cpv]
    )
    return found


def site_updates(system, *options):
    playground, store, builder, _ = system
    return subprocess.run(
        [EGRAPH, "--store", str(store), "--builder", str(builder), *options, "updates"],
        capture_output=True,
        text=True,
        env=dict(playground.settings.environ(), EGRAPH_STRICT="1"),
    )


def test_what_a_tried_flag_reaches_is_evaluated_on_request(
    mutable_playground, tmp_path
):
    system = scenario_system(mutable_playground, tmp_path, "what-if")
    assert site_updates(system).stdout == ""
    result = site_updates(system, "--use", "app-misc/site web")
    assert result.returncode == 0, result.stderr
    rows = [line.split("\t") for line in result.stdout.splitlines()]
    merges = {tuple(fields[:3]) for fields in rows if fields[1] != "tried"}
    assert merges == {
        ("app-misc/site-1", "rebuild", "app-misc/site-1"),
        ("www-apps/server-1", "new", "www-apps/server-1"),
        ("www-apps/lib-1", "new", "www-apps/lib-1"),
    }
    (evaluation,) = evaluations(system)
    assert "www-apps/server" in evaluation.split()


def test_without_refresh_what_a_tried_flag_reaches_is_missing(
    mutable_playground, tmp_path
):
    system = scenario_system(mutable_playground, tmp_path, "what-if")
    assert site_updates(system).returncode == 0
    result = site_updates(system, "--no-refresh", "--use", "app-misc/site web")
    assert result.stderr.startswith(
        "egraph: warning: www-apps/server: reached by what is tried, but not evaluated "
        "(--no-refresh)\n"
    )
    assert evaluations(system) == []


def test_env_files_tried_list_and_rebuild_what_they_change(
    mutable_playground, tmp_path
):
    """An env file tried for a package, and one for every package: the installed packages they
    build otherwise are listed, and rebuilt with --rebuild-env as emerge --reinstall-atoms
    rebuilds them once the lines are saved."""
    import update

    playground = mutable_playground("ledger")
    trees = playground.trees
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    lines = [("env", "app-misc/c withenv.conf"), ("env", "app-misc/a tried/video.conf")]
    options = ["--config-root", playground.eroot]
    for file, text in lines:
        options += [f"--{file}", text]
    listed = egraph(path, *options, "updates", "-D", check=False)
    rows = [line.split("\t") for line in listed.stdout.splitlines()]
    assert [fields for fields in rows if fields[1] == "env"] == [
        ["app-misc/a-1", "env", "", "tried/video.conf"],
        ["app-misc/c-1", "env", "withtest.conf", "withtest.conf withenv.conf"],
    ]
    result = egraph(path, *options, "updates", "-D", "--rebuild-env", check=False)
    save(playground, path, lines)
    _, changed = playground._load_config()
    expected = update.updates(
        changed,
        playground.eroot,
        changed_use=True,
        deep=True,
        reinstall_atoms=("=app-misc/a-1", "=app-misc/c-1"),
    )
    assert expected.success
    assert result.returncode == 0, result.stderr
    assert merged(result.stdout) == (expected.replaced, expected.rebuilt, expected.new)


def test_an_env_file_alone_rebuilds_only_when_asked(mutable_playground, tmp_path):
    """An env file without USE (a compiler) changes no plan, but --rebuild-env adds the
    rebuild, as emerge --reinstall-atoms does."""
    import update

    playground = mutable_playground("what-if")
    env = os.path.join(playground.eroot, "etc", "portage", "env")
    os.makedirs(env, exist_ok=True)
    with open(os.path.join(env, "clang.conf"), "w", encoding="utf-8") as out:
        out.write('CC="clang"\n')
    _, trees = playground._load_config()
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    options = ["--config-root", playground.eroot, "--env", "app-misc/site clang.conf"]
    listed = egraph(path, *options, "updates", "-D")
    rows = [line.split("\t") for line in listed.stdout.splitlines()]
    assert rows == [
        ["", "tried", "none"],
        ["app-misc/site-1", "env", "", "clang.conf"],
    ]
    result = egraph(path, *options, "updates", "-D", "--rebuild-env")
    expected = update.updates(
        trees,
        playground.eroot,
        changed_use=True,
        deep=True,
        reinstall_atoms=("=app-misc/site-1",),
    )
    replaced, rebuilt, new = merged(result.stdout)
    # emerge's reinstall of the same version, for no flag, counts as a plain rebuild.
    rebuilt |= {
        cpv
        for cpv, replacement in replaced.items()
        if replacement.cpv == cpv and replacement.flags is None
    }
    replaced = {cpv: r for cpv, r in replaced.items() if cpv not in rebuilt}
    assert (replaced, rebuilt, new) == (
        expected.replaced,
        expected.rebuilt,
        expected.new,
    )
    assert tried_rows(result.stdout) == {"app-misc/site-1": "added"}
