"""Shadows the C++ REQUIRED_USE check against portage's check_required_use."""

import itertools
import json
import os
import random
import subprocess

import pytest
from portage.dep import check_required_use

SHADOW = os.environ.get("EGRAPH_SHADOW")

pytestmark = pytest.mark.skipif(
    not SHADOW, reason="set EGRAPH_SHADOW to the shadow binary (meson test does)"
)

FLAGS = ("a", "b", "c", "d")


def generated(rng, depth=0):
    """A random, valid REQUIRED_USE term list over FLAGS."""
    terms = []
    for _ in range(rng.randint(0 if depth else 1, 3)):
        kind = rng.choice(
            ("flag", "flag", "group", "op", "cond") if depth < 3 else ("flag",)
        )
        if kind == "flag":
            terms.append(rng.choice(("", "!")) + rng.choice(FLAGS))
        elif kind == "group":
            terms.append(f"( {generated(rng, depth + 1)} )")
        elif kind == "op":
            terms.append(
                f"{rng.choice(('||', '^^', '??'))} ( {generated(rng, depth + 1)} )"
            )
        else:
            flag = rng.choice(("", "!")) + rng.choice(FLAGS)
            terms.append(f"{flag}? ( {generated(rng, depth + 1)} )")
    return " ".join(terms).replace("(  )", "( )")


def shadow(cases):
    """The C++ answers for (required_use, use, empty_true) cases."""
    lines = "".join(
        json.dumps(
            {
                "check": "required_use",
                "tokens": required.split(),
                "use": sorted(use),
                "empty_true": empty_true,
            }
        )
        + "\n"
        for required, use, empty_true in cases
    )
    printed = subprocess.run(
        [SHADOW], input=lines, capture_output=True, text=True, check=True
    ).stdout
    return [json.loads(line) for line in printed.splitlines()]


def portage_answer(required, use, eapi):
    tree = check_required_use(required, frozenset(use), lambda flag: True, eapi=eapi)
    return {"satisfied": bool(tree), "unsatisfied": tree.tounicode()}


def disagreements(strings):
    """Every string under every USE of FLAGS, EAPI 6 (empty groups true) and 8."""
    cases = [
        (required, frozenset(use), eapi)
        for required in strings
        for size in range(len(FLAGS) + 1)
        for use in itertools.combinations(FLAGS, size)
        for eapi in ("6", "8")
    ]
    ours = shadow((required, use, eapi == "6") for required, use, eapi in cases)
    return [
        (case, answer, portage_answer(*case))
        for case, answer in zip(cases, ours)
        if answer != portage_answer(*case)
    ]


def test_generated_required_use_checks_as_portage_does():
    rng = random.Random(16)
    strings = {generated(rng) for _ in range(1500)}
    assert not disagreements(sorted(strings))
