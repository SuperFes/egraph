"""Shadows the C++ dependency reduction against the builder's, portage's use_reduce."""

import itertools
import json
import os
import random
import subprocess

import pytest

from egraph_build import evaluated, installed
from egraph_build.model import DEP_KINDS
from scenarios import SCENARIOS

SHADOW = os.environ.get("EGRAPH_SHADOW")

pytestmark = pytest.mark.skipif(
    not SHADOW, reason="set EGRAPH_SHADOW to the shadow binary (meson test does)"
)

FLAGS = ("a", "b", "c", "d")
# The most USE combinations tried per candidate; beyond, a sample.
COMBINATIONS = 64


def shadow(cases):
    """The C++ node lists, as (type, parent, text) tuples, for (tokens, use, empty_true)."""
    lines = "".join(
        json.dumps(
            {
                "check": "use_reduce",
                "tokens": list(tokens),
                "use": sorted(use),
                "empty_true": empty_true,
            }
        )
        + "\n"
        for tokens, use, empty_true in cases
    )
    printed = subprocess.run(
        [SHADOW], input=lines, capture_output=True, text=True, check=True
    ).stdout
    return [
        [tuple(node) for node in json.loads(line)["nodes"]]
        for line in printed.splitlines()
    ]


def _one(string, use, eapi):
    """The builder's node list for string under use, as installed.dependency_trees lays it out,
    matches aside."""
    from portage.dep import Atom, use_reduce

    tokens = use_reduce(
        string, uselist=use, eapi=eapi, opconvert=False, token_class=Atom
    )
    return [(n.type, n.parent, n.atom) for n in installed.nodes(tokens, lambda a: ())]


def uses(flags, rng):
    """Every subset of flags, or a sample of COMBINATIONS of them."""
    flags = sorted(flags)
    if 2 ** len(flags) <= COMBINATIONS:
        return [
            frozenset(use)
            for size in range(len(flags) + 1)
            for use in itertools.combinations(flags, size)
        ]
    return [
        frozenset(flag for flag in flags if rng.random() < 0.5)
        for _ in range(COMBINATIONS)
    ]


def disagreements(cases):
    """(tokens, use, eapi, empty_true) cases where the C++ nodes are not the builder's."""
    ours = shadow((tokens, use, empty_true) for tokens, use, _, empty_true in cases)
    return [
        (case, answer, expected)
        for case, answer in zip(cases, ours)
        if answer != (expected := _one(" ".join(case[0]), case[1], case[2]))
    ]


def generated(rng, depth=0):
    """A random dependency string over FLAGS, valid for EAPI 8 as long as no group is empty."""
    terms = []
    for _ in range(rng.randint(1, 3)):
        kind = rng.choice(
            ("atom", "atom", "group", "any", "cond") if depth < 3 else ("atom",)
        )
        if kind == "atom":
            atom = rng.choice(("", "!", "!!", ">=")) + "cat/" + rng.choice("pqrs")
            if atom.startswith(">="):
                atom += "-1"
            use = rng.choice(("", "x?", "!x?", "x=", "!x=", "x,-y", "x(+)?,y="))
            if use and not atom.startswith("!"):
                x, y = rng.sample(FLAGS, 2)
                use = use.replace("x", x).replace("y", y)
                atom += f"[{use}]"
            terms.append(atom)
        elif kind == "group":
            terms.append(f"( {generated(rng, depth + 1)} )")
        elif kind == "any":
            terms.append(f"|| ( {generated(rng, depth + 1)} )")
        else:
            flag = rng.choice(("", "!")) + rng.choice(FLAGS)
            terms.append(f"{flag}? ( {generated(rng, depth + 1)} )")
    return " ".join(terms)


def test_generated_dependencies_reduce_as_portage_does():
    rng = random.Random(16)
    strings = sorted({generated(rng) for _ in range(1500)})
    cases = [
        (tuple(string.split()), use, eapi, eapi == "6")
        for string in strings
        for use in uses(FLAGS, rng)
        for eapi in ("6", "8")
    ]
    assert not disagreements(cases)


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_candidates_reduce_as_the_builder_does(playgrounds, name):
    """Every visible candidate, under every USE of its IUSE (or a sample), its own included."""
    system = playgrounds(name)
    portdb = system.trees[system.eroot]["porttree"].dbapi
    layer = evaluated.build(system.vardb, portdb)
    rng = random.Random(16)
    cases = []
    for c in layer.candidates():
        if c.reasons:
            continue
        (eapi,) = portdb.aux_get(c.cpv, ["EAPI"], myrepo=c.repo)
        for use in [frozenset(c.use), *uses(c.iuse, rng)]:
            for tokens in c.tokens:
                if tokens:
                    cases.append((tokens, use, eapi, c.empty_groups_true))
    assert not disagreements(cases)


def test_candidates_own_use_gives_their_stored_nodes(playgrounds):
    """Reduced under its own USE, a candidate's tokens give the nodes the builder stored."""
    for name in sorted(SCENARIOS):
        system = playgrounds(name)
        portdb = system.trees[system.eroot]["porttree"].dbapi
        layer = evaluated.build(system.vardb, portdb)
        visible = [c for c in layer.candidates() if not c.reasons]
        cases = [
            (tokens, frozenset(c.use), c.empty_groups_true)
            for c in visible
            for tokens in c.tokens
        ]
        stored = [
            [(n.type, n.parent, n.atom) for n in nodes]
            for c in visible
            for nodes in c.deps
        ]
        assert shadow(cases) == stored, name
