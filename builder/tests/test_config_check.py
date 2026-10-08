"""egraph config check on every scenario: its findings, and its exit status."""

import os
import subprocess

import portage
import pytest
from conftest import System, portdb, write_index, write_stores
from portage.dep import Atom, match_from_list
from portage.eapi import _get_eapi_attrs
from portage.exception import InvalidAtom
from scenarios import SCENARIOS

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


def package_use_lines(settings):
    """(file, line, atom, flags) for every line of the user's package.use files and its
    profile's, `VAR:` prefixes put on as UseManager puts them."""
    config = os.path.join(
        settings["PORTAGE_CONFIGROOT"], portage.const.USER_CONFIG_PATH
    )
    names = ["package.use"] + [
        os.path.join("profile", name) for name in PROFILE_FILES if ".use" in name
    ]
    found = []
    for name in names:
        for file in files_under(os.path.join(config, name)):
            with open(file, encoding="utf-8") as text:
                for number, line in enumerate(text, 1):
                    words = line.split("#", 1)[0].split()
                    if not words:
                        continue
                    flags, prefix = [], ""
                    for word in words[1:]:
                        if word.endswith(":"):
                            prefix = word[:-1].lower() + "_"
                        elif word.startswith("-"):
                            flags.append("-" + prefix + word[1:])
                        else:
                            flags.append(prefix + word)
                    found.append((file, number, words[0], flags))
    return found


def without_flag(file, number, position):
    """The file's line number without the flag at position, as text."""
    with open(file, encoding="utf-8") as text:
        lines = text.read().splitlines(keepends=True)
    words = lines[number - 1].split("#", 1)[0].split()
    flag_words = [i for i, word in enumerate(words) if i > 0 and not word.endswith(":")]
    del words[flag_words[position]]
    lines[number - 1] = " ".join(words) + "\n"
    return "".join(lines)


def use_states(trees, eroot, packages):
    """{pkg: (PORTAGE_USE, forced or masked of IUSE, whether a flag is in IUSE)}, as setcpv
    decides them."""
    db = trees[eroot]["porttree"].dbapi
    settings = portage.config(clone=trees[eroot]["vartree"].settings)
    found = {}
    for pkg in packages:
        settings.setcpv(pkg, mydb=db)
        iuse_text, eapi = db.aux_get(pkg, ["IUSE", "EAPI"], myrepo=pkg.repo)
        iuse = frozenset(flag.lstrip("+-") for flag in iuse_text.split())
        implicit = (
            settings._iuse_effective_match
            if _get_eapi_attrs(eapi).iuse_effective
            else settings._iuse_implicit_match
        )
        found[pkg] = (
            frozenset(settings["PORTAGE_USE"].split()),
            frozenset((settings.useforce | settings.usemask) & iuse),
            lambda flag, iuse=iuse, implicit=implicit: flag in iuse or implicit(flag),
        )
    return found


def portage_useless_flags(playground):
    """{(file, line, flag): "outside" or "useless"} for each flag of the user's package.use
    lines that matches ebuilds of installed cps: outside the IUSE of them all, or changing
    nothing for any as portage stacks USE without it."""
    trees = playground.trees
    eroot = playground.eroot
    db = trees[eroot]["porttree"].dbapi
    vardb = trees[eroot]["vartree"].dbapi
    candidates = [
        db._pkg_str(cpv, repo)
        for cp in vardb.cp_all()
        for repo in db.getRepositories()
        for cpv in db.cp_list(cp, mytree=db.getRepositoryPath(repo))
    ]
    base = use_states(trees, eroot, candidates)
    found = {}
    for file, number, text, flags in package_use_lines(vardb.settings):
        atom = Atom(text, allow_wildcard=True, allow_repo=True)
        matched = match_from_list(atom, candidates)
        for position, token in enumerate(flags):
            if not matched or "*" in token:
                continue
            flag = token.lstrip("-")
            key = (file, number, token)
            if not any(base[pkg][2](flag) for pkg in matched):
                found[key] = "outside"
                continue
            with open(file, encoding="utf-8") as text_file:
                kept = text_file.read()
            edited = without_flag(file, number, position)
            try:
                with open(file, "w", encoding="utf-8") as out:
                    out.write(edited)
                _, changed_trees = playground._load_config()
                changed = use_states(changed_trees, eroot, matched)
            finally:
                with open(file, "w", encoding="utf-8") as out:
                    out.write(kept)
            state = lambda states, pkg: (flag in states[pkg][0], flag in states[pkg][1])
            if all(state(base, pkg) == state(changed, pkg) for pkg in matched):
                found[key] = "useless"
    return found


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_useless_flags_are_portages(name, mutable_playground, tmp_path):
    playground = mutable_playground(name)
    trees = playground.trees
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    result = check(system, tmp_path, "--layout", "lines")
    found = {}
    for fields in (line.split("\t") for line in result.stdout.splitlines()):
        if fields[3] in ("contradicted", "no-effect") and "package.use" in fields[0]:
            outside = fields[6] == "not in the IUSE of anything it matches"
            found[(fields[0], int(fields[1]), fields[5])] = (
                "outside" if outside else "useless"
            )
    assert found == portage_useless_flags(playground)


# The user's files naming packages that decide visibility, then its profile's.
VISIBILITY_FILES = tuple(
    name for name in USER_FILES if name not in ("package.use", "package.env")
)
PROFILE_VISIBILITY_FILES = (
    "package.mask",
    "package.unmask",
    "package.keywords",
    "package.accept_keywords",
)


def without_unit(file, number, position):
    """The file without the token at position on line number, or without the line (blank, so
    the others keep their numbers) for no position or for its only token."""
    with open(file, encoding="utf-8") as text:
        lines = text.read().splitlines(keepends=True)
    words = lines[number - 1].split("#", 1)[0].split()
    if position is None or len(words) == 2:
        lines[number - 1] = "\n"
    else:
        del words[position + 1]
        lines[number - 1] = " ".join(words) + "\n"
    return "".join(lines)


def portage_useless_visibility(playground):
    """{(file, line, token): "useless"} for each token of the user's visibility lines (each
    mask, unmask and token-less line as a whole, token "") matching versions of installed cps,
    where portage's visibility and reasons of none of them change without it."""
    from test_visibility import portage_view

    trees = playground.trees
    eroot = playground.eroot
    db = trees[eroot]["porttree"].dbapi
    vardb = trees[eroot]["vartree"].dbapi
    installed_cps = set(vardb.cp_all())
    versions = [
        db._pkg_str(cpv, repo)
        for cp in installed_cps
        for repo in db.getRepositories()
        for cpv in db.cp_list(cp, mytree=db.getRepositoryPath(repo))
    ]
    base = portage_view(db)
    config = os.path.join(
        vardb.settings["PORTAGE_CONFIGROOT"], portage.const.USER_CONFIG_PATH
    )
    paths = [os.path.join(config, name) for name in VISIBILITY_FILES]
    paths += [
        os.path.join(config, "profile", name) for name in PROFILE_VISIBILITY_FILES
    ]
    found = {}
    for path in paths:
        whole = os.path.basename(path) in ("package.mask", "package.unmask")
        for file in files_under(path):
            with open(file, encoding="utf-8") as text:
                lines = text.read().splitlines()
            for number, line in enumerate(lines, 1):
                words = line.split("#", 1)[0].split()
                if not words:
                    continue
                atom = Atom(words[0].lstrip("-"), allow_wildcard=True, allow_repo=True)
                keys = [f"{pkg}::{pkg.repo}" for pkg in match_from_list(atom, versions)]
                if not keys:
                    continue
                units = (
                    [(None, "")]
                    if whole or len(words) == 1
                    else list(enumerate(words[1:]))
                )
                for position, token in units:
                    with open(file, encoding="utf-8") as text_file:
                        kept = text_file.read()
                    edited = without_unit(file, number, position)
                    try:
                        with open(file, "w", encoding="utf-8") as out:
                            out.write(edited)
                        _, changed_trees = playground._load_config()
                        changed = portage_view(changed_trees[eroot]["porttree"].dbapi)
                    finally:
                        with open(file, "w", encoding="utf-8") as out:
                            out.write(kept)
                    if all(base[key] == changed[key] for key in keys):
                        found[(file, number, token)] = "useless"
    return found


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_useless_visibility_entries_are_portages(name, mutable_playground, tmp_path):
    playground = mutable_playground(name)
    trees = playground.trees
    system = System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)
    result = check(system, tmp_path, "--layout", "lines")
    found = {
        (fields[0], int(fields[1]), fields[5]): "useless"
        for fields in (line.split("\t") for line in result.stdout.splitlines())
        if fields[3] in ("contradicted", "no-effect")
        and "package.use" not in fields[0]
        and "package.env" not in fields[0]
    }
    assert found == portage_useless_visibility(playground)
