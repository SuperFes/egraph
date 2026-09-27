"""Pins what the oracle means, so a comparison against it tests what we think it tests.

Every expectation here is worked out by hand from PMS, not copied from output.
"""

import pytest

from egraph_build import oracle
from egraph_build.model import Edge, SonameUse


def test_installed(playgrounds):
    vardb = playgrounds("any-of").vardb
    assert oracle.installed(vardb) == (
        "app-misc/x-1",
        "app-misc/y-1",
        "app-misc/z-1",
        "dev-libs/a-1",
        "dev-libs/b-1",
        "dev-libs/c-1",
    )


@pytest.mark.parametrize(
    "atom, expected",
    [
        ("dev-libs/lib[ssl]", ("dev-libs/lib-1",)),
        ("dev-libs/lib[gtk]", ()),
        ("dev-libs/lib[-gtk]", ("dev-libs/lib-1",)),
        # qt is not in IUSE, so the default decides.
        ("dev-libs/lib[qt(+)]", ("dev-libs/lib-1",)),
        ("dev-libs/lib[qt(-)]", ()),
        ("dev-libs/lib[-qt(-)]", ("dev-libs/lib-1",)),
    ],
)
def test_matches_honor_use_deps(playgrounds, atom, expected):
    assert oracle.matches(playgrounds("use-deps").vardb, atom) == expected


def test_conditional_use_deps_follow_the_parents_use(playgrounds):
    vardb = playgrounds("use-deps").vardb
    # The parent has ssl on and gtk off.
    atoms = {
        str(atom) for atom, _ in oracle.dep_atoms(vardb, "app-misc/a-1", "RDEPEND")
    }
    assert "dev-libs/lib[ssl]" in atoms  # [ssl?] and [ssl=]
    assert "dev-libs/lib[-gtk]" in atoms  # [gtk=] and [!gtk?]
    assert "dev-libs/lib" in atoms  # [gtk?]
    assert not any("?" in atom or "=" in atom for atom in atoms)


@pytest.mark.parametrize(
    "atom, expected",
    [
        ("dev-libs/lib:1", ("dev-libs/lib-1.2",)),
        ("dev-libs/lib:2/2.0=", ("dev-libs/lib-2.0",)),
        # Built against a sub-slot that is no longer installed.
        ("dev-libs/lib:1/1.1=", ()),
        ("dev-libs/lib:*", ("dev-libs/lib-1.2", "dev-libs/lib-2.0")),
        (">=dev-libs/lib-2", ("dev-libs/lib-2.0",)),
        ("=dev-libs/lib-1*", ("dev-libs/lib-1.2",)),
        ("~dev-libs/lib-2.0", ("dev-libs/lib-2.0",)),
        ("dev-libs/lib:3", ()),
    ],
)
def test_matches_honor_slots(playgrounds, atom, expected):
    assert oracle.matches(playgrounds("slots").vardb, atom) == expected


def test_deps_keep_kinds_and_mark_choices(playgrounds):
    vardb = playgrounds("any-of").vardb
    x = "app-misc/x-1"
    assert oracle.deps(vardb, x) == {
        Edge(x, "dev-libs/a-1", "RDEPEND", "dev-libs/a", True),
        Edge(x, "dev-libs/b-1", "RDEPEND", "dev-libs/b", True),
        Edge(x, "dev-libs/c-1", "DEPEND", "dev-libs/c", True),
        Edge(x, "dev-libs/a-1", "DEPEND", "dev-libs/a", True),
    }


def test_blockers_are_not_edges(playgrounds):
    vardb = playgrounds("any-of").vardb
    assert oracle.deps(vardb, "app-misc/x-1", kinds=("PDEPEND",)) == frozenset()
    assert oracle.rdeps(playgrounds("reference").vardb, "app-misc/old-1") == frozenset()


def test_use_conditionals_follow_installed_use(playgrounds):
    vardb = playgrounds("reference").vardb
    assert oracle.deps(vardb, "app-misc/root-1", kinds=("DEPEND",)) == {
        Edge("app-misc/root-1", "dev-libs/cond-1", "DEPEND", "dev-libs/cond", False),
    }


def test_rdeps(playgrounds):
    vardb = playgrounds("reference").vardb
    assert oracle.rdeps(vardb, "dev-libs/lib-1") == {
        Edge("app-misc/root-1", "dev-libs/lib-1", "RDEPEND", "dev-libs/lib:1", False),
    }
    assert oracle.rdeps(vardb, "dev-libs/lib-2") == frozenset()
    assert oracle.rdeps(vardb, "app-misc/root-1") == {
        Edge(
            "app-misc/user-1", "app-misc/root-1", "RDEPEND", ">=app-misc/root-1", False
        ),
    }


def test_unparseable_deps_are_errors_not_edges(playgrounds):
    vardb = playgrounds("reference").vardb
    assert oracle.errors(vardb) == {("app-misc/bad-1", "RDEPEND")}
    assert oracle.deps(vardb, "app-misc/bad-1") == frozenset()


def test_malformed_sonames_are_errors(playgrounds):
    vardb = playgrounds("sonames").vardb
    assert oracle.errors(vardb) == {("app-misc/nocategory-1", "REQUIRES")}
    assert oracle.soname_consumers(vardb, "libz.so.1") == {
        SonameUse("dev-libs/openssl-3", "x86_64"),
        SonameUse("app-misc/tool-1", "x86_32"),
    }


def test_emptied_any_of_depends_on_eapi(playgrounds):
    vardb = playgrounds("any-of").vardb
    assert oracle.dep_atoms(vardb, "app-misc/y-1", "DEPEND") == [
        ("__const__/empty-any-of", False)
    ]
    assert oracle.dep_atoms(vardb, "app-misc/z-1", "DEPEND") == []


def test_sonames_keep_multilib_categories(playgrounds):
    vardb = playgrounds("sonames").vardb
    assert oracle.soname_providers(vardb, "libz.so.1") == {
        SonameUse("sys-libs/zlib-1", "x86_32"),
        SonameUse("sys-libs/zlib-1", "x86_64"),
    }
    assert oracle.soname_consumers(vardb, "libz.so.1") == {
        SonameUse("dev-libs/openssl-3", "x86_64"),
        SonameUse("app-misc/tool-1", "x86_32"),
    }
    assert oracle.soname_providers(vardb, "libgone.so.1") == frozenset()
    assert oracle.soname_consumers(vardb, "libgone.so.1") == {
        SonameUse("app-misc/tool-1", "x86_64"),
    }


def test_broken(playgrounds):
    assert oracle.broken(playgrounds("any-of").vardb) == {
        ("app-misc/x-1", "BDEPEND", "|| ( dev-libs/nothing-a dev-libs/nothing-b )"),
        ("app-misc/x-1", "PDEPEND", "dev-libs/absent"),
        ("app-misc/y-1", "DEPEND", "__const__/empty-any-of"),
    }
    assert oracle.broken(playgrounds("slots").vardb) == {
        ("app-misc/a-1", "RDEPEND", "dev-libs/lib:1/1.1="),
        ("app-misc/a-1", "RDEPEND", "dev-libs/lib:3"),
    }
    assert oracle.broken(playgrounds("use-deps").vardb) == {
        ("app-misc/a-1", "RDEPEND", "dev-libs/lib[gtk]"),
        ("app-misc/a-1", "RDEPEND", "dev-libs/lib[qt(-)]"),
    }
