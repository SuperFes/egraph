"""egraph's queries against portage's answers, through the real binary."""

import json
import os
import subprocess

import pytest

from compare import _sonames, possible_mismatches
from conftest import dynamic_option, portdb, write_stores
from egraph_build import oracle, roots
from egraph_build.model import Edge

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)


def egraph(path, *args, check=True, env=None):
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", *args],
        capture_output=True,
        text=True,
        env=None if env is None else {**os.environ, **env},
    )
    if check:
        assert result.returncode == 0, result.stderr
    return result


def parse_edges(text):
    edges = set()
    for line in text.splitlines():
        parent, kind, atom, child, *choice = line.split("\t")
        edges.add(Edge(parent, child, kind, atom, choice == ["any-of"]))
    return frozenset(edges)


@pytest.fixture
def system(scenario, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    return scenario.vardb, path


def test_deps_and_rdeps(scenario, system, dynamic_deps):
    vardb, path = system
    ebuilds = portdb(scenario) if dynamic_deps else None
    option = dynamic_option(dynamic_deps)
    for cpv in oracle.installed(vardb):
        found = parse_edges(egraph(path, "deps", *option, cpv).stdout)
        assert found == oracle.deps(vardb, cpv, portdb=ebuilds), cpv
        found = parse_edges(egraph(path, "rdeps", *option, cpv).stdout)
        assert found == oracle.rdeps(vardb, cpv, portdb=ebuilds), cpv


def parse_possible(text):
    """(edges, possible): deps or rdeps --possible output split into the edges installed
    packages have and the (Edge, flags) pairs toggled flags would add."""
    edges, possible = set(), set()
    for line in text.splitlines():
        parent, kind, atom, child, *markers = line.split("\t")
        edge = Edge(parent, child, kind, atom, "any-of" in markers)
        toggles = [m[len("use=") :] for m in markers if m.startswith("use=")]
        if toggles:
            possible.add((edge, tuple(toggles[0].split())))
        else:
            edges.add(edge)
    return frozenset(edges), frozenset(possible)


def test_possible_deps_and_rdeps(scenario, system):
    vardb, path = system
    ebuilds = portdb(scenario)
    every = set()
    for cpv in oracle.installed(vardb):
        edges, possible = parse_possible(egraph(path, "deps", "--possible", cpv).stdout)
        deps = oracle.deps(vardb, cpv, portdb=ebuilds)
        assert edges == deps, cpv
        assert possible_mismatches(vardb, ebuilds, cpv, possible, deps) == [], cpv
        every |= possible
    for cpv in oracle.installed(vardb):
        edges, possible = parse_possible(
            egraph(path, "rdeps", "--possible", cpv).stdout
        )
        assert edges == oracle.rdeps(vardb, cpv, portdb=ebuilds), cpv
        assert possible == {(edge, flags) for edge, flags in every if edge.child == cpv}


def test_possible_dependencies_name_their_flags(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("possible"), path)
    assert egraph(path, "rdeps", "--possible", "dev-libs/deep").stdout == (
        "app-misc/host-1\tRDEPEND\tdev-libs/deep\tdev-libs/deep-1\tuse=a b c\n"
    )
    assert egraph(path, "rdeps", "--possible", "dev-libs/w2").stdout == (
        "app-misc/host-1\tRDEPEND\tdev-libs/w2\tdev-libs/w2-1\tany-of\tuse=a\n"
    )
    assert egraph(path, "rdeps", "--possible", "dev-libs/z").stdout == (
        "app-misc/host-1\tRDEPEND\tdev-libs/z\tdev-libs/z-1\tuse=-minimal\n"
    )
    # Masked and forced flags stay as they are, and arch flags are the profile's.
    for name in ("m", "f", "arch", "never"):
        assert egraph(path, "rdeps", "--possible", f"dev-libs/{name}").stdout == ""
    result = egraph(
        path, "rdeps", "--possible", "--dynamic-deps", "n", "dev-libs/z", check=False
    )
    assert result.returncode == 2


def parse_updates(text):
    """{installed cpv: (kind, update.Replacement)} from updates output, slot-operator rebuilds
    aside."""
    from compare import rebuild_flag
    from update import Replacement

    found = {}
    for line in text.splitlines():
        cpv, kind, target, repo, *flags = line.split("\t")
        if kind == "new" or len(flags) > 1:
            continue
        names = (
            frozenset(rebuild_flag(flag) for flag in flags[0].split())
            if flags
            else None
        )
        found[cpv] = (kind, Replacement(target, repo, names))
    return found


@pytest.mark.parametrize(
    "option, newuse, changed_use",
    [(None, False, False), ("--newuse", True, False), ("--changed-use", False, True)],
    ids=["update", "newuse", "changed-use"],
)
def test_updates_are_emerges(
    scenario, system, dynamic_deps, option, newuse, changed_use
):
    """What emerge -puD @installed merges: dependents' atoms hold updates back, the deep
    resolution falls back to the best version they accept, as plain -u does not, what the
    merges and the kept packages need that nothing installed provides comes in new, and
    dependents bound to a replaced sub-slot are rebuilt."""
    from portage.versions import cpv_getversion, vercmp

    import update

    expected = update.updates(
        scenario.trees,
        scenario.eroot,
        newuse,
        changed_use,
        deep=True,
        dynamic_deps=dynamic_deps,
    )
    if not expected.success:
        pytest.skip("emerge cannot resolve @installed here")
    _, path = system
    options = [*dynamic_option(dynamic_deps), *filter(None, [option])]
    output = egraph(path, "updates", *options).stdout
    new = {
        fields[0]
        for fields in (line.split("\t") for line in output.splitlines())
        if fields[1] == "new"
    }
    rebuilt = {
        fields[0]
        for fields in (line.split("\t") for line in output.splitlines())
        if fields[1] == "rebuild" and len(fields) > 5
    }
    found = parse_updates(output)
    replaced = {cpv: replacement for cpv, (_, replacement) in found.items()}
    assert (replaced, rebuilt, new) == (
        expected.replaced,
        expected.rebuilt,
        expected.new,
    )
    for cpv, (kind, replacement) in found.items():
        order = vercmp(cpv_getversion(replacement.cpv), cpv_getversion(cpv))
        assert kind == (
            "upgrade" if order > 0 else "downgrade" if order < 0 else "rebuild"
        )


def test_update_order_is_valid(scenario, system, dynamic_deps):
    """updates -t merges what emerge -puD @installed does, each after what it waits for
    wherever emerge's order has it so, and names exactly those of its waits (pending.waits)
    placed before it."""
    from egraph_build import pending

    import update

    expected = update.updates(
        scenario.trees, scenario.eroot, deep=True, dynamic_deps=dynamic_deps
    )
    if not expected.success:
        pytest.skip("emerge cannot resolve @installed here")
    _, path = system
    output = egraph(path, "updates", "-t", *dynamic_option(dynamic_deps)).stdout
    rows = [line.split("\t") for line in output.splitlines()]
    target = {fields[0]: fields[4] for fields in rows}
    place = {fields[4]: int(fields[0]) for fields in rows}
    found = {fields[4]: {target[wait] for wait in fields[1].split()} for fields in rows}
    emerge_place = {cpv: i for i, cpv in enumerate(expected.order)}
    assert set(place) == set(emerge_place)
    db = scenario.trees[scenario.eroot]["porttree"].dbapi
    waits = pending.waits(
        db.settings,
        db,
        None,
        [pending.Entry("ebuild", cpv) for cpv in expected.order],
    )
    for cpv, others in waits.items():
        for other in others:
            if emerge_place[other] < emerge_place[cpv]:
                assert place[other] < place[cpv], (cpv, other)
        assert found[cpv] == {o for o in others if place[o] < place[cpv]}, cpv


def test_updates_by_hand(playgrounds, tmp_path):
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("updates"), path)
    replaced = [
        "app-misc/both-1\tupgrade\tapp-misc/both-2\ttest_repo",
        "app-misc/down-2\tdowngrade\tapp-misc/down-1\ttest_repo",
        "app-misc/gone-2\tdowngrade\tapp-misc/gone-1\ttest_repo",
        "app-misc/past-2\tupgrade\tapp-misc/past-3\ttest_repo",
        "app-misc/rev-1\tupgrade\tapp-misc/rev-1-r1\ttest_repo",
        "app-misc/up-1\tupgrade\tapp-misc/up-2\ttest_repo",
        "dev-libs/slotted-1\tupgrade\tdev-libs/slotted-1.1\ttest_repo",
    ]
    rebuilt = [
        "app-misc/iuse-1\trebuild\tapp-misc/iuse-1\ttest_repo\t(-gone_off%) -new_off%",
        "app-misc/twin-1\trebuild\tapp-misc/twin-1\toverlay\textra%*",
        "app-misc/use-1\trebuild\tapp-misc/use-1\ttest_repo\t"
        "(-gone_off%) (-gone_on%*) -new_off% new_on%* -turned_off* turned_on*",
    ]
    changed = [
        "app-misc/twin-1\trebuild\tapp-misc/twin-1\toverlay\textra%*",
        "app-misc/use-1\trebuild\tapp-misc/use-1\ttest_repo\t"
        "(-gone_on%*) new_on%* -turned_off* turned_on*",
    ]
    assert egraph(path, "updates").stdout.splitlines() == replaced
    assert egraph(path, "updates", "-N").stdout.splitlines() == sorted(
        replaced + rebuilt
    )
    assert egraph(path, "updates", "-U").stdout.splitlines() == sorted(
        replaced + changed
    )
    human = egraph(
        path, "--layout", "human", "--color", "never", "--glyphs", "ascii", "updates"
    ).stdout
    assert human == (
        "U app-misc/both     1 > 2     ::test_repo\n"
        "D app-misc/down     2 > 1     ::test_repo\n"
        "D app-misc/gone     2 > 1     ::test_repo\n"
        "U app-misc/past     2 > 3     ::test_repo\n"
        "U app-misc/rev      1 > 1-r1  ::test_repo\n"
        "U app-misc/up       1 > 2     ::test_repo\n"
        "U dev-libs/slotted  1 > 1.1   ::test_repo\n"
        "\n5 upgrades, 2 downgrades\n"
    )

    def glyphs(locale):
        return egraph(
            path,
            "--layout",
            "human",
            "--color",
            "never",
            "updates",
            env={"LC_ALL": locale},
        ).stdout

    assert glyphs("C") == human
    assert glyphs("xx_XX.UTF-8") == human
    assert glyphs("C.UTF-8").startswith("\uf0aa app-misc/both")


def test_a_cp_names_every_installed_version(playgrounds, tmp_path):
    reference = playgrounds("reference")
    vardb = reference.vardb
    path = tmp_path / "installed.egraph"
    write_stores(reference, path)
    expected = oracle.rdeps(vardb, "dev-libs/lib-1") | oracle.rdeps(
        vardb, "dev-libs/lib-2"
    )
    rdeps = egraph(path, "rdeps", "--dynamic-deps", "n", "dev-libs/lib").stdout
    assert parse_edges(rdeps) == expected
    both = egraph(
        path, "deps", "--dynamic-deps", "n", "app-misc/root-1", "app-misc/user-1"
    ).stdout
    assert parse_edges(both) == oracle.deps(vardb, "app-misc/root-1") | oracle.deps(
        vardb, "app-misc/user-1"
    )


def test_unknown_package(system):
    _, path = system
    result = egraph(path, "rdeps", "app-misc/nonexistent", check=False)
    assert result.returncode == 1
    assert (
        result.stderr == "egraph: app-misc/nonexistent: no installed package matches\n"
    )


def test_sonames(system):
    vardb, path = system
    for soname in _sonames(vardb):
        for flag, query in (
            ([], oracle.soname_consumers),
            (["--providers"], oracle.soname_providers),
        ):
            lines = egraph(path, "soname", *flag, soname).stdout.splitlines()
            assert lines == sorted(
                f"{u.cpv}\t{u.multilib_category}" for u in query(vardb, soname)
            )


def test_broken(scenario, system, dynamic_deps):
    vardb, path = system
    ebuilds = portdb(scenario) if dynamic_deps else None
    expected = sorted("\t".join(item) for item in oracle.broken(vardb, ebuilds))
    found = egraph(path, "broken", *dynamic_option(dynamic_deps)).stdout
    assert found.splitlines() == expected


def test_stats(system):
    vardb, path = system
    lines = dict(
        line.split(": ", 1) for line in egraph(path, "stats").stdout.splitlines()
    )
    cpvs = oracle.installed(vardb)
    assert int(lines["packages"]) == len(cpvs)
    assert int(lines["edges"]) == len(
        frozenset().union(*(oracle.deps(vardb, c) for c in cpvs))
    )
    assert int(lines["unsatisfied"]) == len(oracle.broken(vardb))
    atoms = roots.root_atoms(vardb)
    assert int(lines["root atoms"]) == sum(len(atoms[name]) for name in roots.ROOT_SETS)


def _neighborhood(vardb, roots, depth, forward, reverse):
    seen = set(roots)
    frontier = list(roots)
    for _ in range(depth):
        found = []
        for cpv in frontier:
            if reverse:
                found += [edge.parent for edge in oracle.rdeps(vardb, cpv)]
            if forward:
                found += [edge.child for edge in oracle.deps(vardb, cpv)]
        frontier = [cpv for cpv in dict.fromkeys(found) if cpv not in seen]
        seen.update(frontier)
    return seen


@pytest.mark.parametrize(
    "direction, forward, reverse",
    [("reverse", False, True), ("forward", True, False), ("both", True, True)],
)
@pytest.mark.parametrize("depth", [0, 1, 2, 10])
def test_export_neighborhoods(system, direction, forward, reverse, depth):
    vardb, path = system
    for cpv in oracle.installed(vardb):
        args = ["--depth", str(depth), "--direction", direction, cpv]
        exported = egraph(path, "export", "--format", "json", *args).stdout
        members = {pkg["cpv"] for pkg in json.loads(exported)["packages"]}
        assert members == _neighborhood(vardb, [cpv], depth, forward, reverse)

        dot = egraph(path, "export", *args).stdout
        arrows = {
            tuple(part.strip().strip('"') for part in line.split("[")[0].split("->"))
            for line in dot.splitlines()
            if "->" in line
        }
        induced = {
            (edge.parent, edge.child)
            for member in members
            for edge in oracle.deps(vardb, member)
            if edge.child in members
        }
        assert arrows == induced


def test_the_shell_answers_as_one_shot_commands(scenario, system):
    """One session across commands and option sets, against a fresh process for each."""
    from portage.versions import cpv_getkey

    vardb, path = system
    cpvs = sorted(str(cpv) for cpv in vardb.cpv_all())
    commands = [
        "stats",
        "broken",
        "broken --dynamic-deps n",
        "updates -N --held",
        "updates --dynamic-deps n",
        *(f"{query} {cpv}" for cpv in cpvs[:3] for query in ("deps", "rdeps")),
        f"rdeps --dynamic-deps n {cpvs[0]}",
        f"match --candidates {cpv_getkey(cpvs[0])}",
        "orphans",
        "orphans --with-bdeps n",
        "orphans --dynamic-deps n",
    ]
    expected = "".join(
        egraph(path, *line.split(), check=False).stdout for line in commands
    )
    # Without a command and off a terminal, egraph is the shell.
    for command in (["shell"], []):
        shell = subprocess.run(
            [EGRAPH, "--store", str(path), "--no-refresh", *command],
            input="".join(f"{line}\n" for line in commands),
            capture_output=True,
            text=True,
        )
        assert shell.stdout == expected, command
