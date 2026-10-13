"""Package sets as the builder lists and expands them, against emerge's own set configuration."""

import json

import pytest

from egraph_build import cli, roots

from scenarios import SCENARIOS


def test_every_set_is_listed_as_emerge_knows_it(scenario):
    root_config = scenario.trees[scenario.eroot]["root_config"]
    sets, errors = roots.set_list(scenario.vardb)
    assert [info.name for info in sets] == sorted(root_config.sets)
    for info in sets:
        pset = root_config.sets[info.name]
        assert info.kind == type(pset).__name__
        assert info.description == pset.getMetadata("DESCRIPTION")
        assert info.world_candidate == bool(pset.world_candidate)
    assert errors == tuple(root_config.setconfig.errors)


def test_the_sets_scenario_lists_the_repositorys_and_the_users(playgrounds):
    sets, errors = roots.set_list(playgrounds("sets").vardb)
    found = {info.name: info for info in sets}
    assert {"desktop", "tools", "absent", "mine", "broken"} <= found.keys()
    assert found["desktop"].kind == "StaticFileSet"
    assert found["desktop"].world_candidate
    assert found["live-rebuild"].kind == "VariableSet"
    assert not found["live-rebuild"].world_candidate
    assert errors == ()


def test_a_nested_sets_atoms_go_under_its_own_name(playgrounds):
    desktop, mine = roots.set_atoms(playgrounds("sets").vardb, ["desktop", "mine"])
    assert desktop == roots.SetAtoms(
        "desktop",
        (
            ("desktop", "app-misc/editor"),
            ("desktop", "app-misc/viewer"),
            ("tools", "dev-util/tool"),
        ),
    )
    assert mine == roots.SetAtoms(
        "mine", (("mine", "app-misc/solo"), ("tools", "dev-util/tool"))
    )


def test_portages_own_sets_read_the_vdb_and_the_repositories(playgrounds):
    vardb = playgrounds("sets").vardb
    live, unavailable, downgrade, changed = roots.set_atoms(
        vardb, ["live-rebuild", "unavailable", "downgrade", "changed-deps"]
    )
    assert live.atoms == (("live-rebuild", "app-misc/live:0"),)
    assert unavailable.atoms == (("unavailable", "app-misc/gone:0"),)
    assert downgrade.atoms == (("downgrade", "app-misc/old:0"),)
    # Never anything: ChangedDepsSet compares what strip_slots returns, None.
    assert changed.atoms == ()


def test_a_set_naming_a_missing_set_cannot_be_loaded(playgrounds):
    (broken,) = roots.set_atoms(playgrounds("sets").vardb, ["broken"])
    assert broken.atoms == ()
    assert "nonexistent" in broken.error


def test_an_unknown_set_is_named_as_such(playgrounds):
    (unknown,) = roots.set_atoms(playgrounds("sets").vardb, ["nope"])
    assert unknown == roots.SetAtoms("nope", (), "no such set")


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_each_sets_atoms_are_getsetatoms(playgrounds, name):
    """Every set that loads, on every scenario: its atoms, nested ones included, are those
    portage's set configuration gives it, and each nested set named is one it holds."""
    system = playgrounds(name)
    setconfig = system.trees[system.eroot]["root_config"].setconfig
    sets, _ = roots.set_list(system.vardb)
    names = [info.name for info in sets if info.name != "preserved-rebuild"]
    for found in roots.set_atoms(system.vardb, names):
        if found.error is not None:
            continue
        expected = {str(atom) for atom in setconfig.getSetAtoms(found.name)}
        assert {atom for _, atom in found.atoms} == expected, found.name
        for nested, atom in found.atoms:
            assert atom in {str(a) for a in setconfig.getSets()[nested].getAtoms()}


def test_the_cli_writes_the_list_and_the_atoms(playgrounds, tmp_path):
    system = playgrounds("sets")
    output = tmp_path / "sets.json"
    argv = ["--sets", "--output", str(output), "--config-root", system.eroot]
    argv += ["--eprefix", system.vardb.settings["EPREFIX"]]
    assert cli.main(argv) == cli.EXIT_OK
    listed = json.loads(output.read_text())
    assert listed["errors"] == []
    desktop = next(entry for entry in listed["sets"] if entry["name"] == "desktop")
    assert desktop == {
        "name": "desktop",
        "class": "StaticFileSet",
        "description": desktop["description"],
        "world_candidate": True,
    }
    assert cli.main([*argv, "tools", "nope"]) == cli.EXIT_OK
    assert json.loads(output.read_text())["sets"] == [
        {
            "name": "tools",
            "class": "StaticFileSet",
            "description": desktop["description"].replace("desktop", "tools"),
            "world_candidate": True,
            "atoms": [["tools", "dev-util/tool"]],
        },
        {"name": "nope", "error": "no such set"},
    ]


def test_it_needs_an_output():
    assert cli.main(["--sets"]) == cli.EXIT_USAGE
