"""egraph config check on every scenario: its findings, and its exit status."""

import os
import subprocess

import portage
import pytest
from conftest import portdb, write_index, write_stores
from portage.dep import Atom, match_from_list
from portage.exception import InvalidAtom

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

SEVERITIES = {
    "dead": "error",
    "contradicted": "error",
    "no-effect": "warning",
    "not-installed": "note",
}


def check(system, tmp_path, *options):
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    write_index(system, path)
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", *options, "config", "check"],
        capture_output=True,
        text=True,
    )


def test_findings_are_records_and_errors_fail_the_check(scenario, tmp_path):
    result = check(scenario, tmp_path, "--layout", "lines")
    records = [line.split("\t") for line in result.stdout.splitlines()]
    assert all(len(fields) == 7 for fields in records), result.stdout
    assert all(SEVERITIES[fields[3]] == fields[2] for fields in records)
    errors = any(fields[2] == "error" for fields in records)
    assert result.returncode == (1 if errors else 0), result.stderr


def test_a_clean_configuration_says_so(playgrounds, tmp_path):
    result = check(playgrounds("reference"), tmp_path, "--layout", "human")
    assert (result.returncode, result.stdout) == (0, "no findings\n"), result.stderr


# The files of the user's configuration naming packages, and of its own profile.
USER_FILES = (
    "package.use",
    "package.env",
    "package.mask",
    "package.unmask",
    "package.keywords",
    "package.accept_keywords",
    "package.license",
    "package.properties",
    "package.accept_restrict",
)
PROFILE_FILES = (
    "package.mask",
    "package.unmask",
    "package.keywords",
    "package.accept_keywords",
    "package.use",
    "package.use.force",
    "package.use.mask",
    "package.use.stable",
    "package.use.stable.force",
    "package.use.stable.mask",
)


def files_under(path):
    """path, or the files of the directory path, as portage reads them recursively."""
    if os.path.isfile(path):
        return [path]
    found = []
    for parent, directories, names in os.walk(path):
        directories[:] = sorted(d for d in directories if not d.startswith("."))
        found.extend(
            os.path.join(parent, name)
            for name in sorted(names)
            if not name.startswith(".") and not name.endswith("~")
        )
    return found


def user_lines(settings):
    """(file, line, atom) for every entry of the user's files naming a package."""
    config = os.path.join(
        settings["PORTAGE_CONFIGROOT"], portage.const.USER_CONFIG_PATH
    )
    paths = [os.path.join(config, name) for name in USER_FILES]
    paths += [os.path.join(config, "profile", name) for name in PROFILE_FILES]
    found = []
    for path in paths:
        for file in files_under(path):
            with open(file, encoding="utf-8") as text:
                for number, line in enumerate(text, 1):
                    words = line.split("#", 1)[0].split()
                    if words:
                        found.append((file, number, words[0]))
    return found


def portage_unmatched(vardb, db):
    """{(file, line, atom): kind} for the dead and not-installed entries, from portage's
    matching over every ebuild and installed package."""
    packages = [
        db._pkg_str(cpv, repo)
        for repo in db.getRepositories()
        for cp in db.cp_all(trees=[db.getRepositoryPath(repo)])
        for cpv in db.cp_list(cp, mytree=db.getRepositoryPath(repo))
    ]
    packages += [vardb._pkg_str(cpv, None) for cpv in vardb.cpv_all()]
    installed_cps = set(vardb.cp_all())
    found = {}
    for file, line, text in user_lines(vardb.settings):
        try:
            atom = Atom(text.lstrip("-"), allow_wildcard=True, allow_repo=True)
        except InvalidAtom:
            found[(file, line, text)] = "dead"
            continue
        matched = match_from_list(atom, packages)
        if not matched:
            found[(file, line, text)] = "dead"
        elif not {pkg.cp for pkg in matched} & installed_cps:
            found[(file, line, text)] = "not-installed"
    return found


def unmatched_view(stdout):
    """{(file, line, atom): kind} from check's dead and not-installed records."""
    return {
        (fields[0], int(fields[1]), fields[4]): fields[3]
        for fields in (line.split("\t") for line in stdout.splitlines())
        if fields[3] in ("dead", "not-installed")
    }


def test_dead_and_not_installed_entries_are_portages(scenario, tmp_path):
    result = check(scenario, tmp_path, "--layout", "lines")
    assert unmatched_view(result.stdout) == portage_unmatched(
        scenario.vardb, portdb(scenario)
    )


def test_the_configuration_scenario_has_both_kinds(playgrounds):
    system = playgrounds("config")
    kinds = set(portage_unmatched(system.vardb, portdb(system)).values())
    assert kinds == {"dead", "not-installed"}
