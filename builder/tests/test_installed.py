"""The parts of the installed layer the oracle cannot see: tree shape, blockers,
satisfaction and the canonical JSON. Expectations are worked out by hand."""

import json
import os

import pytest

from egraph_build import installed
from egraph_build.installed import (
    ALL_OF,
    ANY_OF,
    ATOM,
    STRONG_BLOCKER,
    WEAK_BLOCKER,
    Node,
    choices,
    satisfied,
)


@pytest.fixture(scope="module")
def layers(playgrounds):
    return lambda name: installed.build(playgrounds(name).vardb)


def _deps(layer, cpv, kind):
    return layer.package(cpv).deps[installed.DEP_KINDS.index(kind)]


def test_any_of_with_an_all_of_alternative(layers):
    nodes = _deps(layers("any-of"), "app-misc/x-1", "RDEPEND")
    assert nodes == (
        Node(ANY_OF, -1, "", ()),
        Node(ALL_OF, 0, "", ()),
        Node(ATOM, 1, "dev-libs/a", ("dev-libs/a-1",)),
        Node(ATOM, 1, "dev-libs/missing", ()),
        Node(ATOM, 0, "dev-libs/b", ("dev-libs/b-1",)),
    )
    assert choices(nodes) == (False, True, True, True, True)
    assert satisfied(nodes) == (True, False, True, False, True)


def test_nested_any_of_is_flattened_by_portage(layers):
    # || ( gone || ( c a ) ) means || ( gone c a ), and use_reduce says so.
    nodes = _deps(layers("any-of"), "app-misc/x-1", "DEPEND")
    assert nodes == (
        Node(ANY_OF, -1, "", ()),
        Node(ATOM, 0, "dev-libs/gone", ()),
        Node(ATOM, 0, "dev-libs/c", ("dev-libs/c-1",)),
        Node(ATOM, 0, "dev-libs/a", ("dev-libs/a-1",)),
    )
    assert satisfied(nodes) == (True, False, True, True)


def test_nested_any_of_under_all_of():
    nodes = (
        Node(ANY_OF, -1, "", ()),
        Node(ALL_OF, 0, "", ()),
        Node(ANY_OF, 1, "", ()),
        Node(ATOM, 2, "a/x", ()),
        Node(ATOM, 2, "a/y", ("a/y-1",)),
        Node(ATOM, 1, "a/z", ("a/z-1",)),
    )
    assert choices(nodes) == (False, True, True, True, True, True)
    assert satisfied(nodes) == (True, True, True, False, True, True)


def test_unsatisfied_any_of(layers):
    nodes = _deps(layers("any-of"), "app-misc/x-1", "BDEPEND")
    assert satisfied(nodes) == (False, False, False)


def test_unsatisfied_atoms_and_blockers_are_kept(layers):
    nodes = _deps(layers("any-of"), "app-misc/x-1", "PDEPEND")
    assert nodes == (
        Node(ATOM, -1, "dev-libs/absent", ()),
        Node(STRONG_BLOCKER, -1, "!!dev-libs/c", ("dev-libs/c-1",)),
    )
    # A blocker never makes a tree unsatisfied, even when it matches.
    assert satisfied(nodes) == (False, True)


def test_weak_blocker_records_what_it_blocks(layers):
    nodes = _deps(layers("reference"), "dev-libs/lib-1", "RDEPEND")
    assert nodes == (Node(WEAK_BLOCKER, -1, "!app-misc/old", ("app-misc/old-1",)),)


def test_emptied_any_of(layers):
    layer = layers("any-of")
    # EAPI 7 and later: portage substitutes an atom that never matches.
    eapi8 = _deps(layer, "app-misc/y-1", "DEPEND")
    assert eapi8 == (Node(ATOM, -1, "__const__/empty-any-of", ()),)
    assert satisfied(eapi8) == (False,)
    assert _deps(layer, "app-misc/z-1", "DEPEND") == ()


def test_empty_any_of_node_is_satisfied():
    assert satisfied((Node(ANY_OF, -1, "", ()),)) == (True,)


def test_package_fields(layers):
    lib = layers("reference").package("dev-libs/lib-1")
    assert (lib.cp, lib.slot, lib.sub_slot, lib.eapi) == (
        "dev-libs/lib",
        "1",
        "1.2",
        "8",
    )
    alt = layers("reference").package("dev-libs/alt-a-1")
    assert (alt.slot, alt.sub_slot) == ("0", "0")
    root = layers("reference").package("app-misc/root-1")
    assert (root.use, root.iuse) == (("flag",), ("flag",))
    assert root.requires == (("x86_64", "libz.so.1"),)


def test_errors_keep_portages_message(layers):
    bad = layers("reference").package("app-misc/bad-1")
    assert [key for key, _ in bad.errors] == ["RDEPEND"]
    assert bad.errors[0][1]
    assert bad.deps[installed.DEP_KINDS.index("RDEPEND")] == ()
    nocategory = layers("sonames").package("app-misc/nocategory-1")
    assert [key for key, _ in nocategory.errors] == ["REQUIRES"]
    assert nocategory.requires == ()


def test_matches_only_knows_atoms_in_the_trees(layers):
    with pytest.raises(KeyError):
        layers("reference").matches("dev-libs/never-mentioned")


def test_json_is_canonical(layers, playgrounds):
    text = installed.to_json(layers("any-of"))
    assert text == installed.to_json(installed.build(playgrounds("any-of").vardb))
    assert text.endswith("}\n") and "\n" not in text[:-1]
    document = json.loads(text)
    assert document["format"] == 4
    cpvs = [pkg["cpv"] for pkg in document["packages"]]
    assert cpvs == sorted(cpvs)


def test_json_content(layers):
    document = json.loads(installed.to_json(layers("reference")))
    (lib,) = [pkg for pkg in document["packages"] if pkg["cpv"] == "dev-libs/lib-1"]
    # The playground's merge time is when it was set up.
    assert lib.pop("merged") > 0
    assert lib == {
        "cpv": "dev-libs/lib-1",
        "cp": "dev-libs/lib",
        "slot": "1",
        "sub_slot": "1.2",
        "repo": "test_repo",
        "eapi": "8",
        "use": [],
        "iuse": [],
        "errors": [],
        "deps": {
            "BDEPEND": [],
            "DEPEND": [],
            "IDEPEND": [],
            "PDEPEND": [],
            "RDEPEND": [
                {
                    "type": "weak-blocker",
                    "parent": -1,
                    "atom": "!app-misc/old",
                    "matches": ["app-misc/old-1"],
                }
            ],
        },
        "provides": [],
        "requires": [],
        "counter": 0,
    }


def test_a_package_records_its_counter_and_when_it_was_merged(mutable_playground):
    from test_build import add_package, fresh_vardb, vdb

    playground = mutable_playground("reference")
    add_package(playground, "dev-libs/alt-b-1", COUNTER="42")
    os.utime(vdb(playground, "dev-libs/alt-b-1", "COUNTER"), (1759784400, 1759784400))
    pkg = installed.build(fresh_vardb(playground)).package("dev-libs/alt-b-1")
    assert (pkg.counter, pkg.merged) == (42, 1759784400)


def test_an_unreadable_counter_is_zero(mutable_playground):
    from test_build import add_package, fresh_vardb

    playground = mutable_playground("reference")
    add_package(playground, "dev-libs/alt-b-1", COUNTER="lots")
    assert (
        installed.build(fresh_vardb(playground)).package("dev-libs/alt-b-1").counter
        == 0
    )
