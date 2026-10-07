"""GLSAs: the repository index holds them as portage's glsa module parses them, and egraph's
matching of them against the installed packages is held to that module's."""

import os
import random
import subprocess
from xml.sax.saxutils import escape

import portage
import pytest
from conftest import notice_lines, portdb, write_stores
from scenarios import SCENARIOS
from test_build import age, fresh_databases
from test_repository import first_index, rebuilt

from egraph_build import installed, repository, store

EGRAPH = os.environ.get("EGRAPH")

RANGES = ("le", "lt", "eq", "gt", "ge", "rge", "rle", "rgt", "rlt")


def glsa_xml(nr, packages, revision=1, title="a flaw"):
    """A GLSA document; packages are (cp, arch, [(tag, range, version, slot or None)])."""
    affected = []
    for cp, arch, ranges in packages:
        lines = [f'  <package name="{cp}" auto="yes" arch="{arch}">']
        for tag, kind, version, slot in ranges:
            slotted = "" if slot is None else f' slot="{slot}"'
            lines.append(f'   <{tag} range="{kind}"{slotted}>{version}</{tag}>')
        lines.append("  </package>")
        affected.extend(lines)
    body = "\n".join(affected)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE glsa SYSTEM "http://www.gentoo.org/dtd/glsa.dtd">
<glsa id="{nr}">
 <title>{escape(title)}</title>
 <synopsis>Something is wrong.</synopsis>
 <product type="ebuild">thing</product>
 <announced>2026-01-01</announced>
 <revised count="{revision}">2026-01-02</revised>
 <bug>1</bug>
 <access>remote</access>
 <affected>
{body}
 </affected>
 <background><p>Background.</p></background>
 <description><p>Description.</p></description>
 <impact type="normal"><p>Impact.</p></impact>
 <workaround><p>None.</p></workaround>
 <resolution><p>Upgrade.</p></resolution>
 <references><uri link="https://example.org">ref</uri></references>
</glsa>
"""


def write_glsa(directory, nr, packages, **kwargs):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, f"glsa-{nr}.xml"), "w") as f:
        f.write(glsa_xml(nr, packages, **kwargs))


def with_glsa_dir(settings, directory):
    settings = portage.config(clone=settings)
    settings.unlock()
    settings["GLSA_DIR"] = str(directory)
    settings.backup_changes("GLSA_DIR")
    return settings


def test_advisories_are_read_as_the_glsa_module_parses_them(playgrounds, tmp_path):
    system = playgrounds("reference")
    write_glsa(
        tmp_path,
        "202601-02",
        [
            (
                "dev-libs/a",
                "x86 amd64",
                [
                    ("vulnerable", "rlt", "1.2-r3", None),
                    ("vulnerable", "lt", "1.2", "1"),
                    ("unaffected", "ge", "1.2-r3", None),
                ],
            ),
            ("dev-libs/b", "*", [("vulnerable", "rgt", "2-r1", None)]),
        ],
        revision=4,
        title="a & b: flaws",
    )
    write_glsa(tmp_path, "202601-01", [("dev-libs/a", "*", [])])
    # glsa-check skips what it cannot parse, and the glsa module refuses to test a bad arch.
    (tmp_path / "glsa-202601-03.xml").write_text("<glsa")
    write_glsa(tmp_path, "202601-04", [("dev-libs/a", "x86,amd64", [])])
    settings = with_glsa_dir(system.vardb.settings, tmp_path)
    found = repository.read_advisories(settings)
    assert [a.id for a in found] == ["202601-01", "202601-02"]
    advisory = found[1]
    assert (advisory.title, advisory.synopsis, advisory.revision) == (
        "a & b: flaws",
        "Something is wrong.",
        4,
    )
    assert advisory.packages == (
        repository.AdvisoryPackage(
            "dev-libs/a",
            "x86 amd64",
            ("<~dev-libs/a-1.2-r3", "<dev-libs/a-1.2:1"),
            (">=dev-libs/a-1.2-r3",),
        ),
        repository.AdvisoryPackage("dev-libs/b", "*", (">~dev-libs/b-2-r1",), ()),
    )


def test_no_main_repository_is_no_advisories():
    assert repository.read_advisories({}) == ()


def test_the_index_round_trips_with_advisories(playgrounds, tmp_path):
    system = playgrounds("reference")
    write_glsa(
        tmp_path,
        "202601-01",
        [("app-misc/a", "*", [("vulnerable", "lt", "2", None)])],
    )
    index = repository.read(portdb(system))._replace(
        advisories=repository.read_advisories(
            with_glsa_dir(system.vardb.settings, tmp_path)
        )
    )
    assert index.advisories
    meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 7)
    assert store.decode_repository(store.encode_repository(index, meta)) == (
        meta,
        (),
        index,
    )


@pytest.fixture
def repository_playground(mutable_playground):
    playground = mutable_playground("repository")
    age(playground.eroot)
    return playground


def main_glsa_dir(playground):
    _, db = fresh_databases(playground)
    return repository.advisory_directory(db.settings)


def test_a_new_advisory_reads_only_the_advisories_again(repository_playground):
    previous = first_index(repository_playground)
    assert previous[2].advisories == ()
    write_glsa(
        main_glsa_dir(repository_playground),
        "202601-01",
        [("app-misc/a", "*", [("vulnerable", "lt", "2", None)])],
    )
    result = rebuilt(repository_playground, previous)
    assert not result.full
    assert result.reread == frozenset()
    assert [a.id for a in result.index.advisories] == ["202601-01"]


def test_an_unchanged_advisory_directory_is_carried_over(repository_playground):
    directory = main_glsa_dir(repository_playground)
    write_glsa(directory, "202601-01", [("app-misc/a", "*", [])])
    age(directory)
    previous = first_index(repository_playground)
    assert [a.id for a in previous[2].advisories] == ["202601-01"]
    path = os.path.join(repository_playground.eroot, "etc/portage/package.mask")
    with open(path, "a") as f:
        f.write("app-misc/nothing\n")
    result = rebuilt(repository_playground, previous)
    assert not result.full
    assert result.index.advisories == previous[2].advisories


# Shadowing egraph's matching against the glsa module's.


def _versions_near(version):
    """Versions around an installed one: itself, other revisions of it, and its neighbours."""
    base, _, revision = version.partition("-r")
    found = {version, base, f"{base}-r1", f"{base}-r2", f"{base}-r10"}
    found.add(f"{base}.1")
    parts = base.split(".")
    if parts[0].isdigit():
        found.add(str(int(parts[0]) + 1))
        if int(parts[0]) > 0:
            found.add(str(int(parts[0]) - 1))
    return sorted(v for v in found if portage.versions.ververify(v))


def corpus(vardb, seed):
    """GLSAs whose ranges sit around the installed versions, every range kind and arch rule."""
    rng = random.Random(seed)
    arch = vardb.settings["ARCH"]
    packages = []
    for cpv in sorted(vardb.cpv_all()):
        cp = portage.cpv_getkey(cpv)
        version = cpv[len(cp) + 1 :]
        slot = vardb.aux_get(cpv, ["SLOT"])[0].partition("/")[0]
        packages.append((cp, slot, _versions_near(version)))
    if not packages:
        return []
    found = []
    for n in range(240):
        cp, slot, versions = rng.choice(packages)
        arches = rng.choice(("*", arch, f"amd64 {arch}", "sparc"))

        def ranges(tag, count):
            chosen = []
            for _ in range(count):
                kind = rng.choice(RANGES)
                version = rng.choice(versions)
                if kind.startswith("r") and "-r" not in version:
                    version += "-r0" if rng.random() < 0.3 else "-r1"
                # The glsa module fails on a slotted revision range.
                with_slot = not kind.startswith("r") and rng.random() < 0.3
                chosen.append(
                    (
                        tag,
                        kind,
                        version,
                        rng.choice((slot, "*", "99")) if with_slot else None,
                    )
                )
            return chosen

        entry = ranges("vulnerable", rng.randint(1, 2)) + ranges(
            "unaffected", rng.randint(0, 2)
        )
        found.append((f"2026{n // 100 + 1:02d}-{n % 100:02d}", [(cp, arches, entry)]))
    return found


def portage_affected(settings, vardb, db, ids, applied):
    """(id, cpv) for each installed version a GLSA affects, by the glsa module's own match."""
    from portage import glsa

    found = set()
    for nr in ids:
        if nr in applied:
            continue
        advisory = glsa.Glsa(nr, settings, vardb, db)
        affected = set()
        for paths in advisory.packages.values():
            for path in paths:
                if path["arch"] != "*" and settings["ARCH"] not in path["arch"].split():
                    continue
                vulnerable = {
                    str(cpv)
                    for atom in path["vul_atoms"]
                    for cpv in glsa.match(atom, vardb)
                }
                unaffected = {
                    str(cpv)
                    for atom in path["unaff_atoms"]
                    for cpv in glsa.match(atom, vardb)
                }
                affected |= vulnerable - unaffected
        assert advisory.isVulnerable() == bool(affected), nr
        found.update((nr, cpv) for cpv in affected)
    return found


@pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_egraph_finds_the_advisories_the_glsa_module_does(name, playgrounds, tmp_path):
    system = playgrounds(name)
    vardb = system.vardb
    db = portdb(system)
    generated = corpus(vardb, name)
    glsa_dir = tmp_path / "glsa"
    for nr, packages in generated:
        write_glsa(glsa_dir, nr, packages)
    applied = {nr for nr, _ in generated[::7]}
    eroot = tmp_path / "root"
    os.makedirs(eroot / "var/lib/portage")
    (eroot / "var/lib/portage/glsa_injected").write_text(
        "".join(f"{nr}\n" for nr in sorted(applied))
    )
    settings = with_glsa_dir(vardb.settings, glsa_dir)
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    index = repository.read(db)._replace(
        advisories=repository.read_advisories(settings)
    )
    assert len(index.advisories) == len(generated)
    meta = store.RepositoryMeta("0.0.0", "3.0.0", str(eroot), 0)
    store.write(store.repository_path(path), store.encode_repository(index, meta))
    lines = notice_lines(system, path, tmp_path)
    found = {
        (row[0], row[3])
        for row in (line.split("\t") for line in lines)
        if row[1] == "glsa"
    }
    ids = [nr for nr, _ in generated]
    assert found == portage_affected(settings, vardb, db, ids, applied)


@pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)
def test_cpp_repository_export_holds_the_advisories(playgrounds, tmp_path):
    system = playgrounds("reference")
    write_glsa(
        tmp_path / "glsa",
        "202601-01",
        [
            (
                "app-misc/a",
                "x86 amd64",
                [("vulnerable", "rlt", "2-r1", None), ("unaffected", "ge", "2", "0")],
            )
        ],
        revision=3,
        title='quotes " and <tags>',
    )
    path = tmp_path / "installed.egraph"
    store.write(
        path, store.encode(installed.build(system.vardb), store.Meta("0", "0", "/", 0))
    )
    index = repository.read(portdb(system))._replace(
        advisories=repository.read_advisories(
            with_glsa_dir(system.vardb.settings, tmp_path / "glsa")
        )
    )
    meta = store.RepositoryMeta("0.0.0", "3.0.0", "/", 0)
    store.write(store.repository_path(path), store.encode_repository(index, meta))
    result = subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "export", "--format", "json"]
        + ["--repository"],
        capture_output=True,
    )
    assert result.stderr == b""
    assert result.stdout == repository.to_json(index).encode()
