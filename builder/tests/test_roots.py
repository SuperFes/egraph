"""Root sets as the builder records them, against portage's set configuration."""

from egraph_build import installed, roots


def test_roots_scenario_sets(playgrounds):
    layer = installed.build(playgrounds("roots").vardb)
    assert [(r.set, r.atom, r.matches) for r in layer.roots()] == [
        ("selected", "app-misc/set-member", ("app-misc/set-member-1",)),
        ("selected", "app-misc/world", ("app-misc/world-1",)),
        (
            "selected",
            "sys-kernel/sources",
            ("sys-kernel/sources-1", "sys-kernel/sources-2"),
        ),
        ("system", "sys-apps/base", ("sys-apps/base-1",)),
        ("profile", "app-misc/prof", ("app-misc/prof-1",)),
    ]


def test_roots_are_emerges_sets(scenario):
    """The sets depclean starts from, read through emerge's own root_config."""
    setconfig = scenario.trees[scenario.eroot]["root_config"].setconfig
    expected = [
        (name, str(atom), tuple(sorted(scenario.vardb.match(atom))))
        for name in roots.ROOT_SETS
        for atom in sorted(setconfig.getSetAtoms(name), key=str)
    ]
    layer = installed.build(scenario.vardb)
    assert [tuple(root) for root in layer.roots()] == expected
