"""egraph's queries against portage's answers, through the real binary."""

import json
import os
import subprocess

import pytest
from portage.versions import cpv_getkey, cpv_getversion, vercmp

from compare import _sonames, possible_mismatches
from conftest import dynamic_option, portdb, write_stores
from egraph_build import oracle, roots
from egraph_build.cli import EXIT_REFUSED
from egraph_build.model import Edge
from scenarios import SCENARIOS

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
    # A plan emerge would refuse for its blockers is still a plan.
    if check:
        assert result.returncode in (0, EXIT_REFUSED), result.stderr
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


# The rows after the merges: blockers, and what refuses the plan.
TRAILING_KINDS = (
    "uninstall",
    "blocks",
    "unsatisfied",
    "required-use",
    "use-change",
    "masked",
    "tried",
    "env",
)


# A new package's kinds: alone in its cp, or beside installed packages in other slots.
NEW_KINDS = ("new", "new-slot")


def merge_lines(text):
    """updates or plan output without its held updates (listed when nothing satisfies them),
    uninstalls, blocks and refusals."""
    return [
        line
        for line in text.splitlines()
        if line.split("\t")[1] not in ("held", *TRAILING_KINDS)
    ]


def blocker_rows(text):
    """(uninstalled cpvs, {(atom without its "!"s, holder cpv)}) from updates or plan output,
    as update.updates has them."""
    uninstalls, blocks = set(), set()
    for line in text.splitlines():
        fields = line.split("\t")
        if fields[1] == "uninstall":
            uninstalls.add(fields[0])
        elif fields[1] == "blocks":
            blocks.add((fields[2].lstrip("!"), fields[0]))
    return frozenset(uninstalls), frozenset(blocks)


def unsatisfied_rows(text):
    """The atoms of the unsatisfied rows in updates or plan output."""
    return frozenset(
        fields[2]
        for fields in (line.split("\t") for line in text.splitlines())
        if fields[1] == "unsatisfied"
    )


def unmet_rows(text):
    """The cpv::repo of the required-use rows in updates or plan output."""
    return frozenset(
        f"{fields[0]}::{fields[2]}"
        for fields in (line.split("\t") for line in text.splitlines())
        if fields[1] == "required-use"
    )


def use_change_rows(text):
    """{cpv::repo: flags} of the use-change rows in updates or plan output."""
    return {
        f"{fields[0]}::{fields[2]}": frozenset(fields[3].split()[1:])
        for fields in (line.split("\t") for line in text.splitlines())
        if fields[1] == "use-change"
    }


def refusal_agrees(text, expected):
    """Whether egraph refuses the plan where emerge does: for blockers, for dependencies nothing
    satisfies or REQUIRED_USE unmet, naming at least what emerge names (it stops at the first it
    finds), or for USE changes, the same ones."""
    unsatisfied, unmet = unsatisfied_rows(text), unmet_rows(text)
    if expected.unmet:
        return expected.unmet <= unmet
    if expected.unsatisfied:
        return expected.unsatisfied <= unsatisfied
    return (
        not unsatisfied
        and not unmet
        and use_change_rows(text) == (expected.use_changes or {})
    )


def refuses(expected):
    return (
        expected.blocked
        or bool(expected.unsatisfied)
        or bool(expected.unmet)
        or bool(expected.use_changes)
    )


def blockers_agree(rows, expected):
    """Whether blocker_rows are emerge's: the same blocks, and the same uninstalls unless it
    refuses the plan, when it lists none."""
    uninstalls, blocks = rows
    return blocks == expected.blocks and (
        expected.blocked or uninstalls == expected.uninstalls
    )


def parse_updates(text):
    """{installed cpv: (kind, update.Replacement)} from updates output, slot-operator rebuilds
    aside."""
    from compare import rebuild_flag
    from update import Replacement

    found = {}
    for line in merge_lines(text):
        cpv, kind, target, repo, *flags = line.split("\t")
        if kind in NEW_KINDS or len(flags) > 1:
            continue
        names = (
            frozenset(rebuild_flag(flag) for flag in flags[0].split())
            if flags
            else None
        )
        found[cpv] = (kind, Replacement(target, repo, names))
    return found


def new_use(text):
    """{new cpv: its USE} from updates or plan output."""
    found = {}
    for line in merge_lines(text):
        cpv, kind, *fields = line.split("\t")
        if kind in NEW_KINDS:
            found[cpv] = fields[2] if len(fields) > 2 else ""
    return found


def merged(text):
    """What updates output merges, as update.updates' replaced, rebuilt and new."""
    lines = [line.split("\t") for line in merge_lines(text)]
    replaced = {
        cpv: replacement for cpv, (_, replacement) in parse_updates(text).items()
    }
    rebuilt = {
        fields[0] for fields in lines if fields[1] == "rebuild" and len(fields) > 5
    }
    new = {fields[0] for fields in lines if fields[1] in NEW_KINDS}
    return replaced, rebuilt, new


def new_slots_agree(vardb, text):
    """Whether each new package is new-slot exactly when emerge shows it NS (its cp installed in
    another slot), listing those installed packages as "cpv:slot", by version."""
    from portage.versions import cpv_getkey

    for line in merge_lines(text):
        cpv, kind, *fields = line.split("\t")
        if kind not in NEW_KINDS:
            continue
        installed = vardb.match(cpv_getkey(cpv))
        beside = " ".join(
            f"{other}:{vardb.aux_get(other, ['SLOT'])[0].split('/')[0]}"
            for other in installed
        )
        if (kind == "new-slot") != bool(installed):
            return False
        if installed and fields[4:] != [beside]:
            return False
    return True


@pytest.mark.parametrize(
    "option, newuse, changed_use",
    [(None, False, False), ("--newuse", True, False), ("--changed-use", False, True)],
    ids=["update", "newuse", "changed-use"],
)
@pytest.mark.parametrize("target", ["@installed", "@world"])
@pytest.mark.parametrize("deep", [False, True], ids=["u", "uD"])
def test_updates_are_emerges(
    scenario, system, dynamic_deps, option, newuse, changed_use, target, deep
):
    """What emerge -puD @installed merges: dependents' atoms hold updates back, the deep
    resolution falls back to the best version they accept, what the merges and the kept
    packages need that nothing installed provides comes in new, and dependents bound to a
    replaced sub-slot are rebuilt. With --world, what emerge -puD @world merges: only what the
    root sets reach is updated, and only it weighs. Without -D, what plain -pu merges: only the
    arguments are updated, and what their merges need; an update anything rejects is dropped,
    with no fallback and no rebuilds."""
    from portage.versions import cpv_getversion, vercmp

    import update

    expected = update.updates(
        scenario.trees,
        scenario.eroot,
        newuse,
        changed_use,
        deep=deep,
        target=target,
        dynamic_deps=dynamic_deps,
    )
    if not expected.success and not refuses(expected):
        pytest.skip(f"emerge cannot resolve {target} here")
    _, path = system
    world = ["--world"] if target == "@world" else []
    options = [
        *dynamic_option(dynamic_deps),
        *filter(None, [option]),
        *world,
        *(["-D"] if deep else []),
    ]
    result = egraph(path, "updates", *options, check=False)
    assert result.returncode == (
        EXIT_REFUSED if refuses(expected) else 0
    ), result.stderr
    output = result.stdout
    assert refusal_agrees(output, expected)
    # Its merge list stops at what it refuses for, or misses what a USE change pulls in when
    # the changed package was in its graph already: test_use_changes_once_made_are_emerges.
    if expected.unsatisfied or expected.unmet or expected.use_changes:
        return
    assert blockers_agree(blocker_rows(output), expected)
    assert merged(output) == (expected.replaced, expected.rebuilt, expected.new)
    assert new_use(output) == expected.use
    assert new_slots_agree(scenario.vardb, output)
    assert masked_rows(output) == expected.masked
    for cpv, (kind, replacement) in parse_updates(output).items():
        order = vercmp(cpv_getversion(replacement.cpv), cpv_getversion(cpv))
        assert kind == (
            "upgrade" if order > 0 else "downgrade" if order < 0 else "rebuild"
        )


def masked_rows(output):
    """The installed cpvs updates says emerge warns are masked."""
    return frozenset(
        line.split("\t")[0] for line in output.splitlines() if "\tmasked\t" in line
    )


def plan_requests(name):
    """What a scenario is asked for: each cp, each ebuild's =cpv and slot atom, and the root
    sets."""
    from portage.versions import cpv_getkey

    scenario = SCENARIOS[name]
    requests = {"@world", "@selected", "@system"}
    for key in ("ebuilds", "installed"):
        for cpv in scenario.get(key, {}):
            requests.add(cpv_getkey(cpv.split("::")[0]))
    for cpv, metadata in scenario.get("ebuilds", {}).items():
        cpv = cpv.split("::")[0]
        requests.add(f"={cpv}")
        if "SLOT" in metadata:
            requests.add(f"{cpv_getkey(cpv)}:{metadata['SLOT'].split('/')[0]}")
    return sorted(requests - set(scenario.get("unrequested", ())))


def plan_merges(text):
    """(installed cpv it replaces or "", cpv, repo) for each merge in plan output."""
    merges = set()
    for line in merge_lines(text):
        cpv, kind, target, repo, *_ = line.split("\t")
        merges.add(("" if kind in NEW_KINDS else cpv, target, repo))
    return frozenset(merges)


def _tied(a, b):
    if a == b:
        return True
    return (
        bool(a and b)
        and cpv_getkey(a) == cpv_getkey(b)
        and vercmp(cpv_getversion(a), cpv_getversion(b)) == 0
    )


def ties(got, expected):
    """Whether two sets of plan_merges are the same but for the choice among equal versions,
    which emerge makes in directory order (a stable sort after os.listdir)."""
    left = list(expected)
    for merge in got:
        match = next(
            (
                other
                for other in left
                if merge[2] == other[2]
                and all(_tied(a, b) for a, b in zip(merge[:2], other[:2]))
            ),
            None,
        )
        if match is None:
            return False
        left.remove(match)
    return not left


def test_equal_versions_tie():
    one = frozenset({("dev-libs/v-1.0", "dev-libs/v-1.0", "test_repo")})
    same = frozenset({("dev-libs/v-1.00", "dev-libs/v-1.00", "test_repo")})
    assert ties(one, same)
    assert not ties(one, {("dev-libs/v-1.01", "dev-libs/v-1.01", "test_repo")})
    assert not ties(one, {("dev-libs/v-1.00", "dev-libs/v-1.00", "overlay")})
    assert not ties(one, one | same)
    assert ties({("", "dev-libs/v-1.0", "r")}, {("", "dev-libs/v-1.00", "r")})
    assert not ties(
        {("", "dev-libs/v-1.0", "r")}, {("dev-libs/v-1", "dev-libs/v-1.0", "r")}
    )


PLAN_MODES = {
    "u": ({"update": True}, ["-u"]),
    "uD": ({"update": True, "deep": True}, ["-u", "-D"]),
    "plain": ({"update": False}, []),
    "n": ({"update": False, "noreplace": True}, ["-n"]),
}


@pytest.mark.parametrize("mode", sorted(PLAN_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_plans_are_emerges(playgrounds, tmp_path, name, mode):
    """What emerge --pretend merges for each request, with -u, -uD, -n or neither: atoms, slot
    atoms, =cpv and sets."""
    import update

    system = playgrounds(name)
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    options, flags = PLAN_MODES[mode]
    differences = set()
    for target in plan_requests(name):
        expected = update.updates(
            system.trees, system.eroot, target=[target], **options
        )
        result = egraph(path, "plan", *flags, target, check=False)
        if not expected.success and not refuses(expected):
            continue
        if result.returncode == 1 and target in expected.unsatisfied:
            # Nothing installed or visible matches the argument, which egraph refuses up front.
            assert "matches" in result.stderr, target
            continue
        assert result.returncode == (EXIT_REFUSED if refuses(expected) else 0), (
            target,
            result.stderr,
        )
        assert refusal_agrees(result.stdout, expected), target
        if expected.unsatisfied or expected.unmet or expected.use_changes:
            continue
        if not ties(plan_merges(result.stdout), expected.merges):
            differences.add(target)
        assert blockers_agree(blocker_rows(result.stdout), expected), target
        assert new_slots_agree(system.vardb, result.stdout), target
        use = new_use(result.stdout)
        for cpv in use.keys() & expected.use.keys():
            assert use[cpv] == expected.use[cpv], (target, cpv)
    assert not differences


def removals(text):
    """{held cpv: (wanted cpv, holder cpvs, world atoms, freed cpvs)} for each held update in
    updates --held output that removing its holders lets through, and {held cpv: wanted cpv}
    for every held one."""
    wanted, holders, found = {}, {}, {}
    for line in text.splitlines():
        cpv, kind, *fields = line.split("\t")
        if kind == "held":
            wanted[cpv] = fields[0]
        elif kind == "holder":
            holders.setdefault(cpv, []).append(fields)
        elif kind == "remove":
            found[cpv] = (
                wanted[cpv],
                {holder[0] for holder in holders[cpv]},
                {
                    root.split(" ", 1)[1]
                    for holder in holders[cpv]
                    for root in holder[2:]
                    if root.startswith("@selected ")
                },
                fields[0].split(),
            )
    return found, wanted


@pytest.mark.parametrize("name", sorted(SCENARIOS))
@pytest.mark.parametrize("target", ["@installed", "@world"])
@pytest.mark.parametrize("option, newuse", [(None, False), ("--newuse", True)])
@pytest.mark.parametrize("deep", [False, True], ids=["u", "uD"])
def test_removals_let_updates_through(
    name, target, option, newuse, deep, playgrounds, tmp_path
):
    """Each removal updates --held offers is emerge's: without the holders, and with their
    world atoms deselected, emerge -pu (-D as asked) merges the held update (named, in case
    nothing keeps it any more) and every other one the removal frees, wherever emerge can
    resolve that system at all."""
    import update
    from conftest import make_playground

    path = tmp_path / "installed.egraph"
    write_stores(playgrounds(name), path)
    world = ["--world"] if target == "@world" else []
    options = [*filter(None, [option]), *world, *(["-D"] if deep else [])]
    output = egraph(path, "updates", "--held", *options).stdout
    found, wanted = removals(output)
    for held, (target_cpv, holders, atoms, frees) in found.items():
        playground = make_playground(name, removed=holders, deselected=atoms)
        try:
            resolves = update.updates(
                playground.trees, playground.eroot, newuse, deep=deep, target=target
            ).success
            expected = update.updates(
                playground.trees,
                playground.eroot,
                newuse,
                deep=deep,
                target=[target, f"={target_cpv}"],
            )
        finally:
            playground.cleanup()
        if not resolves:
            continue
        assert expected.success, held
        for cpv in [held, *frees]:
            assert expected.replaced[cpv].cpv == wanted[cpv], (held, cpv)


def test_a_leaf_holder_is_offered_for_removal(playgrounds, tmp_path):
    """A holder only world keeps, one nothing keeps, and one whose rebuild would hold the
    update, freeing another, can go for it (test_removals_let_updates_through asks emerge).
    """
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("bounds"), path)
    lines = egraph(path, "updates", "--held", "-D").stdout.splitlines()
    held = ("dev-libs/astroid-4.0.4", "dev-libs/lone-1", "app-misc/rgb-1_rc3")
    remedies = [
        line
        for line in lines
        if line.split("\t")[0] in held
        and line.split("\t")[1] in ("holder", "remove", "nodeps")
    ]
    assert remedies == [
        "app-misc/rgb-1_rc3\tholder\tapp-misc/skin-1\t\t@selected app-misc/skin",
        "app-misc/rgb-1_rc3\tremove\tapp-misc/effects-1",
        "app-misc/rgb-1_rc3\tnodeps",
        "dev-libs/astroid-4.0.4\tholder\tapp-misc/pylint-1\t\t@selected app-misc/pylint",
        "dev-libs/astroid-4.0.4\tremove\t",
        "dev-libs/astroid-4.0.4\tnodeps",
        "dev-libs/lone-1\tholder\tapp-misc/stray-1\t",
        "dev-libs/lone-1\tremove\t",
        "dev-libs/lone-1\tnodeps",
    ]


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
    output = egraph(path, "updates", "-t", "-D", *dynamic_option(dynamic_deps)).stdout
    # Merges only: the rows after them lead with an empty place.
    rows = [fields for line in output.splitlines() if (fields := line.split("\t"))[0]]
    target = {fields[0]: fields[4] for fields in rows}
    place = {fields[4]: int(fields[0]) for fields in rows}
    # The earlier merges each waits for by a dependency of its own.
    found = {
        fields[4]: {
            target[wait.rstrip("birplt")]
            for wait in fields[1].split()
            if set(wait) & set("bir") and int(wait.rstrip("birplt")) < int(fields[0])
        }
        for fields in rows
    }
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


def test_update_tree_follows_why(scenario, system, dynamic_deps):
    """updates --tree leads each merge of updates -t down from its root: why's chain for an
    installed package it replaces, and for a new one the chain of the member that pulled it in
    (the installed package, or the one a merge replaces)."""
    _, path = system
    options = dynamic_option(dynamic_deps)
    # The merges, without the uninstalls and blocks after them.
    table = [
        line.split("\t")
        for line in egraph(path, "updates", "-t", *options).stdout.splitlines()
        if not line.startswith("\t")
    ]
    tree = [
        line.split("\t")
        for line in egraph(path, "updates", "--tree", *options).stdout.splitlines()
    ]
    assert [fields[0] for fields in tree] == [fields[0] for fields in table]
    # The installed cpv a candidate cpv replaces, for a new package's puller.
    replaced = {fields[4]: fields[2] for fields in table if fields[3] not in NEW_KINDS}
    for row, fields in zip(table, tree):
        chain = fields[2:]
        assert chain[-1] == row[2]
        if row[3] in NEW_KINDS:
            puller = row[7].split(" ")[0]
            if len(chain) > 1:
                assert chain[-2] in (puller, replaced.get(puller)), row
            continue
        why = egraph(path, "why", row[2], *options, check=False)
        if why.returncode != 0:
            assert fields[1:] == ["", row[2]]
            continue
        lines = [line.split("\t") for line in why.stdout.splitlines()]
        assert fields[1] == lines[0][0]
        assert chain == [lines[0][2]] + [edge[3] for edge in lines[1:]]


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


def test_blockers(scenario, system, dynamic_deps):
    """Without packages, the blockers that match something installed; of a package, those it
    holds and those holding it back."""
    vardb, path = system
    ebuilds = portdb(scenario) if dynamic_deps else None
    found = oracle.blockers(vardb, ebuilds)
    option = dynamic_option(dynamic_deps)
    output = egraph(path, "blockers", *option).stdout
    assert output.splitlines() == sorted("\t".join(item) for item in found if item[3])
    for cpv in oracle.installed(vardb):
        output = egraph(path, "blockers", *option, f"={cpv}").stdout
        expected = {item for item in found if item[0] == cpv or item[3] == cpv}
        assert output.splitlines() == sorted("\t".join(item) for item in expected), cpv


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


@pytest.mark.parametrize("mode", sorted(PLAN_MODES))
def test_use_changes_once_made_are_emerges(playgrounds, tmp_path, mode):
    """Each plan egraph refuses for USE changes is the plan emerge makes once package.use makes
    them: the same merges, and nothing refused."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    import update
    from conftest import System, playground_arguments

    name = "usechange"
    system = playgrounds(name)
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    options, flags = PLAN_MODES[mode]
    checked = 0
    for target in plan_requests(name):
        result = egraph(path, "plan", *flags, target, check=False)
        lines = [
            row.split("\t")[3]
            for row in result.stdout.splitlines()
            if row.split("\t")[1] == "use-change"
        ]
        if not lines:
            continue
        checked += 1
        arguments = playground_arguments(name)
        arguments["user_config"] = {"package.use": tuple(lines)}
        changed = ResolverPlayground(**arguments)
        try:
            trees = changed.trees
            expected = update.updates(trees, changed.eroot, target=[target], **options)
        finally:
            changed.cleanup()
        assert expected.success, (target, lines)
        assert ties(plan_merges(result.stdout), expected.merges), target
    assert checked


def test_masked_installed_packages_are_warned_of(playgrounds, tmp_path):
    """The repository scenario's license-masked package is warned of whatever the graph holds,
    with its reasons; its package.mask one emerge keeps only where the graph holds it, with the
    comment above the entry; and the one it downgrades never."""
    import update

    scenario = playgrounds("repository")
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    for args, deep, target, pinned in (
        ((), False, "@installed", True),
        (("--world", "-D"), True, "@world", False),
    ):
        output = egraph(path, "updates", *args).stdout
        expected = update.updates(
            scenario.trees, scenario.eroot, deep=deep, target=target
        )
        assert masked_rows(output) == expected.masked
        rows = {
            line.split("\t")[0]: line.split("\t")[2:]
            for line in output.splitlines()
            if "\tmasked\t" in line
        }
        assert rows["app-misc/eula-1"] == ["test_repo", "EULA license(s)", ""]
        assert "app-misc/masked-2" not in rows
        assert ("app-misc/pinned-1" in rows) == pinned
    repo, reasons, filename, *comment = rows_of(path)["app-misc/pinned-1"]
    assert (reasons, comment) == (
        "package.mask",
        ["# A Developer <dev@example.org> (2026-10-02)", "# Masked for testing."],
    )
    assert filename.endswith("/etc/portage/package.mask")


def rows_of(path):
    """updates' masked rows, by cpv: the fields after the kind."""
    output = egraph(path, "updates").stdout
    return {
        line.split("\t")[0]: line.split("\t")[2:]
        for line in output.splitlines()
        if "\tmasked\t" in line
    }
