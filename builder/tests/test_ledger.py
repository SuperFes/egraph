"""The USE ledger: every source of a flag's state, entry by entry with its file and line, held
to what portage loaded."""

import os
import re
import subprocess

import pytest
from portage.dep import Atom
from portage.versions import cpv_getkey

from conftest import portdb, write_index, write_stores

from egraph_build import evaluated, ledger

EGRAPH = os.environ.get("EGRAPH")


def entries_of(use_ledger):
    for node in use_ledger.profiles:
        for source in node.sources:
            yield from source
    for repo in use_ledger.repositories:
        for source in repo.sources:
            yield from source
    yield from use_ledger.conf
    yield from use_ledger.package_use
    yield from use_ledger.package_env
    for _, entries in use_ledger.env_files:
        yield from entries
    yield from use_ledger.env
    yield from use_ledger.env_d


def line_of(entry):
    with open(entry.file, encoding="utf-8", errors="replace") as f:
        return f.readlines()[entry.line - 1]


def test_every_entry_is_on_its_line(scenario):
    """Line 0 is where a source fell back to portage's values: none of the scenarios' do."""
    use_ledger = ledger.read(portdb(scenario).settings)
    for entry in entries_of(use_ledger):
        if not entry.file:
            continue
        assert entry.line > 0, entry
        text = line_of(entry)
        if entry.atom:
            assert text.split()[0] == entry.atom, entry
        elif entry.var == "USE" and "=" not in text:
            assert text.split() == list(entry.tokens), entry
        else:
            assert re.match(rf"\s*(export\s+)?{entry.var}=", text), entry


def tuple_of(entries):
    return tuple(token for entry in entries for token in entry.tokens)


def dict_of(entries):
    keys = {}
    for entry in entries:
        keys.setdefault(entry.atom, []).extend(entry.tokens)
    return [(atom, tuple(tokens)) for atom, tokens in keys.items()]


def flat(held):
    return sorted(ledger._flatten(held))


def test_sources_are_portages(scenario):
    settings = portdb(scenario).settings
    manager = settings._use_manager
    use_ledger = ledger.read(settings)
    for i, node in enumerate(use_ledger.profiles):
        defaults, *rest = node.sources
        assert list(tuple_of(defaults)) == settings.make_defaults_use[i].split()
        for (name, attribute, _), entries in zip(ledger.KINDS[1:], rest):
            held = getattr(manager, attribute)[i]
            if name.startswith("package."):
                assert sorted(dict_of(entries)) == flat(held), name
            else:
                assert tuple_of(entries) == tuple(held), name
    for repo in use_ledger.repositories:
        defaults, *rest = repo.sources
        made = settings._repo_make_defaults.get(repo.name, {})
        assert {e.var: list(e.tokens) for e in defaults} == {
            var: made[var].split() for var in made if var in {e.var for e in defaults}
        }
        for (name, _, attribute), entries in zip(ledger.KINDS[1:], rest):
            held = getattr(manager, attribute).get(repo.name, {})
            if name.startswith("package."):
                assert sorted(dict_of(entries)) == flat(held), name
            else:
                assert tuple_of(entries) == tuple(held), name
    assert sorted(dict_of(use_ledger.package_use)) == flat(manager._pusedict)
    assert sorted(dict_of(use_ledger.package_env)) == flat(settings._penvdict)


def test_the_ledger_scenario(playgrounds):
    system = playgrounds("ledger")
    use_ledger = ledger.read(portdb(system).settings)

    def local(entries):
        return [
            (os.path.relpath(e.file, system.eroot), e.line, e.atom, e.var, e.tokens)
            for e in entries
        ]

    assert use_ledger.use_expand == ("LINGUAS", "VIDEO_CARDS")
    assert use_ledger.arch == "x86"
    profile, user = use_ledger.profiles[-2:]
    sources = dict(zip(ledger.FILES, profile.sources))
    made = os.path.relpath(os.path.join(profile.path, "make.defaults"), system.eroot)
    assert local(sources["make.defaults"]) == [
        (made, 6, "", "LINGUAS", ("linguas_en",)),
        (made, 5, "", "VIDEO_CARDS", ("video_cards_vesa", "video_cards_intel")),
        (made, 4, "", "USE", ("pdefault", "-pminus", "conf")),
    ]
    assert [e.tokens for e in sources["use.force"]] == [("x86",), ("pforce",)]
    assert [e.tokens for e in sources["use.stable"]] == [("stableflag",)]
    assert [e.tokens for e in sources["use.stable.mask"]] == [("stablemasked",)]
    assert [(e.line, e.atom, e.tokens) for e in sources["package.use"]] == [
        (1, "app-misc/a", ("pkgflag",)),
        (2, ">=app-misc/a-2", ("-pkgflag",)),
    ]
    assert user.path == os.path.join(system.eroot, "etc", "portage", "profile")
    assert [e.tokens for e in user.sources[ledger.FILES.index("use.mask")]] == [
        ("-umask",)
    ]

    # The playground writes make.conf lines of its own before the scenario's.
    with open(os.path.join(system.eroot, "etc/portage/make.conf")) as f:
        use = 1 + next(i for i, text in enumerate(f) if text.startswith("USE="))
    assert local(use_ledger.conf) == [
        ("etc/portage/make.conf", use, "", "USE", ("-pdefault", "linguas_*")),
        ("etc/portage/make.conf", use + 1, "", "VIDEO_CARDS", ("radeon",)),
        ("etc/portage/package.use/00-base", 2, "*/*", "USE", ("glob",)),
        ("etc/portage/env/everywhere.conf", 1, "", "USE", ("globalenv",)),
    ]
    base, more = "etc/portage/package.use/00-base", "etc/portage/package.use/10-more"
    assert local(use_ledger.package_use) == [
        (
            base,
            4,
            "app-misc/a",
            "USE",
            ("user", "-conf", "-video_cards_*", "video_cards_nvidia"),
        ),
        (base, 5, "app-misc/a:0", "USE", ("slotflag",)),
        (more, 1, "=app-misc/a-2", "USE", ("exact",)),
        (more, 2, "app-misc/a", "USE", ("more",)),
        (more, 3, ">=app-misc/a-1", "USE", ("ranged", "-glob")),
        (more, 4, "<app-misc/a-3", "USE", ("-ranged",)),
    ]
    assert local(use_ledger.package_env) == [
        ("etc/portage/package.env", 2, "app-misc/b", "USE", ("withenv.conf",)),
        ("etc/portage/package.env", 3, "app-misc/c", "USE", ("withtest.conf",)),
    ]
    assert [(name, local(entries)) for name, entries in use_ledger.env_files] == [
        (
            "withenv.conf",
            [("etc/portage/env/withenv.conf", 1, "", "USE", ("fromenv",))],
        ),
        ("withtest.conf", []),
    ]
    assert use_ledger.env == ()


def test_a_source_that_differs_is_kept_as_portage_holds_it(tmp_path):
    """A line portage reads differently than the lines say (here a flag it rejects, and one
    no line names) falls back to portage's tokens at line 0."""
    path = tmp_path / "package.use"
    path.write_text("app-misc/a good b@d\n")
    atom = Atom("app-misc/a")
    held = {"app-misc/a": {atom: ("good",)}}
    assert ledger._dict_source(str(path), False, held) == (
        ledger.Entry(str(path), 1, "app-misc/a", "USE", ("good",)),
    )
    held = {"app-misc/a": {atom: ("good", "other")}}
    assert ledger._dict_source(str(path), False, held) == (
        ledger.Entry(str(path), 0, "app-misc/a", "USE", ("good", "other")),
    )
    path = tmp_path / "use.mask"
    path.write_text("a\nb c\n")
    assert ledger._tuple_source(str(path), False, ("a",)) == (
        ledger.Entry(str(path), 1, "", "USE", ("a",)),
    )
    assert ledger._tuple_source(str(path), False, ("a", "d")) == (
        ledger.Entry(str(path), 0, "", "USE", ("a", "d")),
    )


def test_candidates_carry_their_own_layers(playgrounds):
    system = playgrounds("ledger")
    layer, _ = evaluated.rebuild(system.vardb, portdb(system), None, None)
    by_cpv = {c.cpv: c for c in layer.candidates()}
    assert by_cpv["app-misc/a-1"].stable
    assert not by_cpv["app-misc/a-2"].stable
    assert by_cpv["app-misc/a-1"].internal == ("idefault", "repomasked")
    assert by_cpv["app-misc/b-1"].features == ("test",)
    assert by_cpv["app-misc/c-1"].internal == ("idefault", "-test")
    assert by_cpv["app-misc/c-1"].features == ("test",)
    assert by_cpv["app-misc/a-1"].eapi == "8"
    assert by_cpv["app-misc/a-1"].iuse_effective
    assert layer.ledger() == ledger.read(portdb(system).settings)


def test_make_conf_linked_from_etc_is_read_once(tmp_path):
    (tmp_path / "etc" / "portage").mkdir(parents=True)
    (tmp_path / "etc" / "portage" / "make.conf").write_text('USE="a"\n')
    (tmp_path / "etc" / "make.conf").symlink_to("portage/make.conf")
    files = ledger._make_conf_files({"PORTAGE_CONFIGROOT": str(tmp_path)})
    assert files == [str(tmp_path / "etc" / "make.conf")]


def egraph_use(path, package="*/*"):
    """{cpv::repo: {flag: (enabled, forced, where)}} as egraph use --all lists them."""
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "use", "--all", package],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    found = {}
    for line in result.stdout.splitlines():
        pkg, shown, _, _, where, _ = line.split("\t")
        forced = shown.startswith("(")
        shown = shown.strip("()")
        found.setdefault(pkg, {})[shown.lstrip("-")] = (
            not shown.startswith("-"),
            forced,
            where,
        )
    return found


@pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)
def test_stacked_use_is_portages(scenario, tmp_path):
    """egraph's stacking of the ledger gives every candidate the USE and forced flags portage
    gives it."""
    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    layer, _ = evaluated.rebuild(scenario.vardb, portdb(scenario), None, None)
    if not layer.candidates():
        pytest.skip("no ebuilds in this scenario")
    found = egraph_use(path)
    wrong = []
    for c in layer.candidates():
        flags = found.get(f"{c.cpv}::{c.repo}", {})
        use = sorted(flag for flag, (on, _, _) in flags.items() if on)
        forced = sorted(flag for flag, (_, fixed, _) in flags.items() if fixed)
        if use != sorted(c.use) or forced != sorted(c.forced):
            wrong.append(
                f"{c.cpv}::{c.repo}: portage {sorted(c.use)} {sorted(c.forced)}, "
                f"egraph {use} {forced}"
            )
    assert not wrong, "\n".join(wrong)


def test_candidates_use_is_emerges(scenario):
    """Each candidate's USE and forced flags as emerge's Package has them, its repository's own
    profile files included (setcpv given a cpv string leaves those out)."""
    from _emerge.Package import Package

    db = portdb(scenario)
    root_config = scenario.trees[scenario.eroot]["root_config"]
    layer, _ = evaluated.rebuild(scenario.vardb, db, None, None)
    wrong = []
    for c in layer.candidates():
        if c.reasons:
            continue
        keys = list(Package.metadata_keys)
        metadata = dict(zip(keys, db.aux_get(c.cpv, keys, myrepo=c.repo)))
        metadata["repository"] = c.repo
        pkg = Package(
            built=False,
            cpv=c.cpv,
            installed=False,
            metadata=metadata,
            root_config=root_config,
            type_name="ebuild",
        )
        iuse = set(c.iuse)
        use = sorted(pkg.use.enabled)
        forced = sorted(iuse & (pkg.use.force | pkg.use.mask))
        if use != sorted(c.use) or forced != sorted(c.forced):
            wrong.append(
                f"{c.cpv}::{c.repo}: emerge {use} {forced}, "
                f"builder {sorted(c.use)} {sorted(c.forced)}"
            )
    assert not wrong, "\n".join(wrong)


@pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)
def test_use_shows_what_emerge_would_build_and_what_is_installed(scenario, tmp_path):
    """Without --all: the installed versions' own ebuilds, the best visible version of each
    installed slot (what an update builds) and of each cp (what emerge <cp> picks), of one
    version the one of the repository portage puts first."""
    from portage.versions import vercmp

    path = tmp_path / "installed.egraph"
    write_stores(scenario, path)
    write_index(scenario, path)
    layer, _ = evaluated.rebuild(scenario.vardb, portdb(scenario), None, None)
    if not layer.candidates():
        pytest.skip("no ebuilds in this scenario")
    order = list(portdb(scenario).getRepositories())
    installed = {
        (cpv, scenario.vardb.aux_get(cpv, ["repository"])[0])
        for cpv in scenario.vardb.cpv_all()
    }
    installed_slots = {
        (cpv_getkey(cpv), scenario.vardb.aux_get(cpv, ["SLOT"])[0].split("/")[0])
        for cpv in scenario.vardb.cpv_all()
    }

    def best_of(group):
        best = {}
        for c in layer.candidates():
            if c.reasons:
                continue
            version, key = c.cpv[len(c.cp) + 1 :], f"{c.cpv}::{c.repo}"
            held = best.get(group(c))
            newer = 1 if held is None else vercmp(version, held[0])
            if newer > 0 or (newer == 0 and order.index(c.repo) < order.index(held[1])):
                best[group(c)] = (version, c.repo, key)
        return best

    by_slot = best_of(lambda c: (c.cp, c.slot))
    expected = (
        {key for slot, (_, _, key) in by_slot.items() if slot in installed_slots}
        | {key for _, _, key in best_of(lambda c: c.cp).values()}
        | {
            f"{c.cpv}::{c.repo}"
            for c in layer.candidates()
            if (c.cpv, c.repo) in installed
        }
    )
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "use", "*/*"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    shown = {line.split("\t")[0] for line in result.stdout.splitlines()}
    # An ebuild without flags lists none.
    with_flags = {f"{c.cpv}::{c.repo}" for c in layer.candidates() if c.iuse or c.use}
    assert shown == expected & with_flags


@pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)
def test_use_leaves_out_slots_nothing_is_installed_in(playgrounds, tmp_path):
    """sources-1 and -2 are installed and -3 is what emerge picks; slot 0 has neither."""
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("world-slots"), path)
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "use", "sys-kernel/sources"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    shown = sorted({line.split("\t")[0] for line in result.stdout.splitlines()})
    assert shown == [f"sys-kernel/sources-{v}::test_repo" for v in (1, 2, 3)]


@pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)
def test_a_mask_taken_back_sets_nothing(playgrounds, tmp_path):
    """The user profile's -umask unmasks umask, which nothing turns on: the flag's line names no
    step, while its steps show the unmask, which changed nothing."""
    path = tmp_path / "installed.egraph"
    write_stores(playgrounds("ledger"), path)

    def lines(*arguments):
        result = subprocess.run(
            [EGRAPH, "--store", str(path), "--no-refresh", "use", *arguments],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
        return [line.split("\t") for line in result.stdout.splitlines()]

    (row,) = [row for row in lines("=app-misc/a-1") if row[1] == "-umask"]
    assert row[2:] == ["iuse", "", "", ""]
    steps = lines("=app-misc/a-1", "umask")
    assert [(s[2], s[3], s[6], s[7]) for s in steps] == [
        ("off", "mask", "-umask", "unchanged")
    ]
    assert steps[0][4].endswith("/etc/portage/profile/use.mask:1")
