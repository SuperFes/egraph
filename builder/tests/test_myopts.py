"""Shadows the C++ emerge_myopts, the options of the resume entry --resume-list writes, against
emerge's own parse_opts, over every option egraph passes to emerge."""

import json
import os
import random
import subprocess

import pytest

SHADOW = os.environ.get("EGRAPH_SHADOW")

pytestmark = pytest.mark.skipif(
    not SHADOW, reason="set EGRAPH_SHADOW to the shadow binary (meson test does)"
)

# Each option with the values it is passed with, None for none: the execution options as
# execution_options writes them, the request's, and the roots.
VALUES = {
    "--alert": (None, "y", "n", "True"),
    "--ask": ("n", "y"),
    "--buildpkg": (None, "y", "n", "True"),
    "--buildpkg-exclude": ("a/b", "c/d e/f"),
    "--color": ("y", "n"),
    "--fail-clean": (None, "y", "n"),
    "--jobs": (None, "1", "4", "y", "n"),
    "--jobs-tmpdir-require-free-gb": ("0", "18"),
    "--keep-going": (None, "y", "n", "True"),
    "--load-average": (None, "0", "2", "3.5", "True"),
    "--nospinner": (None,),
    "--quiet": (None, "y", "n"),
    "--quiet-build": (None, "y", "n", "True"),
    "--quiet-fail": (None, "y", "n"),
    "--update": (None,),
    "--deep": (None, "0", "2"),
    "--noreplace": (None,),
    "--newuse": (None,),
    "--changed-use": (None,),
    "--dynamic-deps": ("y", "n"),
    "--oneshot": (None,),
    "--ignore-default-opts": (None,),
    "--root": ("/", "/mnt/gentoo"),
    "--config-root": ("/c/",),
    "--prefix": ("/p",),
}


def spelled(name, value):
    return name if value is None else f"{name}={value}"


def cases():
    single = [
        [spelled(name, value)] for name, values in VALUES.items() for value in values
    ]
    rng = random.Random(16)
    names = sorted(VALUES)
    mixed = []
    for _ in range(300):
        chosen = [rng.choice(names) for _ in range(rng.randint(2, 8))]
        mixed.append([spelled(name, rng.choice(VALUES[name])) for name in chosen])
    return single + mixed


def emerges(options):
    """emerge's myopts for options."""
    from _emerge.main import parse_opts

    _, myopts, _ = parse_opts(list(options), silent=True)
    return myopts


def shadow(corpus):
    lines = "".join(
        json.dumps({"check": "myopts", "options": options}) + "\n" for options in corpus
    )
    printed = subprocess.run(
        [SHADOW], input=lines, capture_output=True, text=True, check=True
    ).stdout
    return [json.loads(line) for line in printed.splitlines()]


def test_myopts_are_emerges():
    corpus = cases()
    ours = shadow(corpus)
    assert len(ours) == len(corpus)
    differ = [
        (options, theirs, mine)
        for options, mine in zip(corpus, ours)
        if (theirs := emerges(options)) != mine
    ]
    assert not differ, differ[:5]
