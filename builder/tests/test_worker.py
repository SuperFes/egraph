"""egraph-build --worker: each package built and merged as emerge -1 builds and merges it."""

import bz2
import difflib
import json
import os
import shutil
import stat
import subprocess
import sys

import pytest

from egraph_build import cli, worker
from test_actions import BUILDER_DIR, merge_environment, portage_program

INSTALL = """S="${WORKDIR}"
pkg_pretend() { einfo "pretend ${PF}"; }
src_install() {
	echo "installing to stdout"
	insinto /usr/share/${PN}
	echo "${PV}" > "${T}"/version
	doins "${T}"/version
	dosym version /usr/share/${PN}/link
	keepdir /var/lib/${PN}
	insinto /etc
	echo "${PV}" > "${T}"/${PN}.conf
	doins "${T}"/${PN}.conf
	if use doc; then
		insinto /usr/share/doc/${PF}
		doins "${T}"/version
	fi
	if [[ ${PV} == 1 ]]; then
		insinto /usr/share/${PN}
		doins "${T}"/${PN}.conf
	fi
}
pkg_preinst() { einfo "replacing: ${REPLACING_VERSIONS}"; }
pkg_postinst() {
	echo "${PF} after ${REPLACING_VERSIONS:-nothing}" >> "${EROOT}"/var/lib/${PN}/postinst
	elog "Merged ${PF}."
}
"""

PLAIN = {"EAPI": "8", "KEYWORDS": "x86"}
EBUILDS = {
    # The second replaces the first: what only the first installed goes, the configuration file
    # each installs waits as ._cfg once the user's differs.
    "app-misc/files-1": {**PLAIN, "IUSE": "+doc extra", "MISC_CONTENT": INSTALL},
    "app-misc/files-2": {**PLAIN, "IUSE": "+doc extra", "MISC_CONTENT": INSTALL},
    "app-misc/older-1": {
        "EAPI": "7",
        "KEYWORDS": "x86",
        "IUSE": "doc",
        "MISC_CONTENT": INSTALL,
    },
    "app-misc/lib-1": {**PLAIN, "IUSE": "doc", "MISC_CONTENT": INSTALL},
    # Asks for lib as it builds, which a worker merged a request before.
    "app-misc/needs-1": {
        **PLAIN,
        "IUSE": "doc",
        "DEPEND": "app-misc/lib",
        "MISC_CONTENT": INSTALL
        + 'pkg_setup() { has_version app-misc/lib || die "lib not seen"; }\n',
    },
    "app-misc/broken-1": {
        **PLAIN,
        "MISC_CONTENT": 'S="${WORKDIR}"\nsrc_compile() { die "cannot compile"; }\n',
    },
}

# Where the image is not the merge's: the playground's own repository, distfiles, fake
# programs and configuration, the build, cache and log directories either run writes, and the
# news emerge checks for after its run.
NOT_IMAGE = (
    "bin",
    "etc/portage",
    "var/lib/gentoo",
    "var/cache",
    "var/log",
    "var/portage",
    "var/repositories",
    "var/tmp",
    "var/db/pkg",
)

# What two merges of a package record differently however alike they are: when it was built,
# and when its entry was written (in the single metadata file, as the files they were).
VOLATILE_VDB = ("BUILD_TIME", "#dir_mtime")


@pytest.fixture
def playground(gnupg_home):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    made = ResolverPlayground(ebuilds=EBUILDS)
    yield made
    made.cleanup()


class Machine:
    """A playground merged into by emerge -1 or by a worker, from the same state each time."""

    def __init__(self, playground, tmp_path):
        self.playground = playground
        self.eprefix = playground.settings["EPREFIX"]
        self.eroot = playground.settings["EROOT"]
        self.environment = dict(os.environ, **merge_environment(playground))
        self.environment["PYTHONPATH"] = (
            BUILDER_DIR + os.pathsep + self.environment["PYTHONPATH"]
        )
        self.snapshot = str(tmp_path / "snapshot")
        self.tmp_path = tmp_path

    def save(self):
        shutil.copytree(self.eprefix, self.snapshot, symlinks=True)

    def restore(self):
        shutil.rmtree(self.eprefix)
        shutil.copytree(self.snapshot, self.eprefix, symlinks=True)

    def emerge(self, *args):
        result = subprocess.run(
            f"{portage_program('emerge')} --ask=n --color=n --nospinner "
            + " ".join(args),
            shell=True,
            env=self.environment,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        return result

    def worker(self, *lines):
        """The worker's events for lines, each request's in turn, and its exit status."""
        result = subprocess.run(
            [sys.executable, "-m", "egraph_build", "--worker"],
            input="".join(line + "\n" for line in lines),
            env=self.environment,
            capture_output=True,
            text=True,
        )
        events = [json.loads(line) for line in result.stdout.splitlines()]
        return events, result.returncode, result.stderr

    def path(self, *parts):
        return os.path.join(self.eroot, *parts)

    def installed(self, cpv):
        return os.path.isdir(self.path("var/db/pkg", cpv))

    def state(self):
        """What a merge leaves: the image, the world file among it, and every vdb entry."""
        return {"image": self.image(), "vdb": self.vdb()}

    def image(self):
        found = {}
        for directory, dirs, files in os.walk(self.eroot):
            relative = os.path.relpath(directory, self.eroot)
            if relative != "." and relative.startswith(NOT_IMAGE):
                dirs.clear()
                continue
            for name in dirs + files:
                path = os.path.join(directory, name)
                key = os.path.normpath(os.path.join(relative, name))
                if key.startswith(NOT_IMAGE):
                    continue
                found[key] = describe(path)
        return found

    def vdb(self):
        found = {}
        top = self.path("var/db/pkg")
        for directory, dirs, files in os.walk(top):
            for name in files:
                path = os.path.join(directory, name)
                key = os.path.relpath(path, top)
                if name in VOLATILE_VDB:
                    continue
                if name == "metadata":
                    found[key] = metadata(path)
                    continue
                with open(path, "rb") as f:
                    content = f.read()
                if name == "environment.bz2":
                    found[key] = (
                        bz2.decompress(content).decode(errors="replace").splitlines()
                    )
                elif name == "CONTENTS":
                    found[key] = contents(content)
                else:
                    found[key] = content
        return found


def describe(path):
    info = os.lstat(path)
    if stat.S_ISLNK(info.st_mode):
        return ("link", os.readlink(path))
    mode = stat.S_IMODE(info.st_mode)
    if stat.S_ISDIR(info.st_mode):
        return ("dir", mode)
    with open(path, "rb") as f:
        return ("file", mode, f.read())


def metadata(path):
    """The vdb entry's metadata file without its volatile lines."""
    with open(path) as f:
        return [
            line
            for line in f.read().splitlines()
            if not line.startswith(tuple(f"{name}=" for name in VOLATILE_VDB))
        ]


def contents(content):
    """CONTENTS without the mtimes, which say when it was merged."""
    lines = []
    for line in content.decode().splitlines():
        fields = line.split(" ")
        if fields[0] in ("obj", "sym"):
            fields = fields[:-1]
        lines.append(" ".join(fields))
    return lines


def differences(emerged, worked):
    """What the worker left otherwise than emerge, path by path, a line diff where there are
    lines."""
    lines = []
    for part in ("image", "vdb"):
        for path in sorted(set(emerged[part]) | set(worked[part])):
            before, after = emerged[part].get(path), worked[part].get(path)
            if before == after:
                continue
            lines.append(f"{part} {path}:")
            if isinstance(before, list) and isinstance(after, list):
                lines += difflib.unified_diff(before, after, lineterm="", n=0)
            else:
                lines += [f"  emerge {before!r}", f"  worker {after!r}"]
    return "\n".join(lines)


def request(cpv, repo="test_repo"):
    return json.dumps({"cpv": cpv, "repo": repo})


@pytest.fixture
def machine(playground, tmp_path):
    return Machine(playground, tmp_path)


def merged_as_emerge(machine, before, targets):
    """After the cpvs before, merged by emerge: the targets merged one by one by emerge -1, and
    all by one worker, leave the same system; the worker's events for them."""
    for cpv in before:
        machine.emerge("-1", f"={cpv}")
    # The user's configuration file, which each version's waits beside.
    os.makedirs(machine.path("etc"), exist_ok=True)
    with open(machine.path("etc", "files.conf"), "a") as f:
        f.write("the user's\n")
    machine.save()
    for cpv in targets:
        machine.emerge("-1", f"={cpv}")
    emerged = machine.state()
    machine.restore()
    events, status, stderr = machine.worker(*map(request, targets))
    assert status == 0, stderr
    worked = machine.state()
    assert worked == emerged, differences(emerged, worked)
    return events


@pytest.mark.parametrize(
    "before, targets, facts",
    [
        (
            [],
            ["app-misc/files-1"],
            {
                "etc/._cfg0000_files.conf": "1\n",
                "usr/share/doc/files-1/version": "1\n",
                "var/lib/files/postinst": "files-1 after nothing\n",
            },
        ),
        (
            ["app-misc/files-1"],
            ["app-misc/files-2"],
            {
                "etc/._cfg0000_files.conf": "2\n",
                "usr/share/files/files.conf": None,
                "var/db/pkg/app-misc/files-1": None,
                "var/lib/files/postinst": "files-1 after nothing\nfiles-2 after 1\n",
            },
        ),
        (
            [],
            ["app-misc/older-1"],
            {"etc/older.conf": "1\n", "usr/share/doc/older-1/version": None},
        ),
        ([], ["app-misc/lib-1", "app-misc/needs-1"], {}),
    ],
    ids=["new", "replacing", "eapi-7", "after-another"],
)
def test_a_worker_merges_as_emerge_does(machine, before, targets, facts):
    """The image, the vdb entries and the world file the worker leaves are emerge -1's: a new
    package, one replacing an installed version, an older EAPI, and one building against what
    the same worker merged just before. Each case's facts, a path's content or None where
    nothing is, hold what it is there to cover."""
    events = merged_as_emerge(machine, before, targets)
    expected = []
    for cpv in targets:
        expected += [{"phase": phase} for phase in worker.PHASES]
        expected.append({"merged": cpv})
    assert events == expected
    for path, content in facts.items():
        if content is None:
            assert not os.path.lexists(machine.path(path)), path
        else:
            with open(machine.path(path)) as f:
                assert f.read() == content, path


def test_a_failed_phase_merges_nothing_and_names_its_log(machine):
    events, status, stderr = machine.worker(
        request("app-misc/broken-1"), request("app-misc/lib-1")
    )
    assert status == 0, stderr
    compile = worker.PHASES.index("compile")
    assert events[: compile + 1] == [{"phase": p} for p in worker.PHASES[: compile + 1]]
    failed = events[compile + 1]
    assert failed["failed"] == "compile"
    assert failed["status"] != 0
    with open(failed["log"]) as f:
        assert "cannot compile" in f.read()
    assert not machine.installed("app-misc/broken-1")
    # The worker goes on to the next request.
    assert events[-1] == {"merged": "app-misc/lib-1"}
    assert machine.installed("app-misc/lib-1")


@pytest.mark.parametrize(
    "line",
    [
        "not json",
        "[]",
        '{"cpv": "app-misc/lib-1"}',
        '{"cpv": "app-misc/lib-1", "repo": 1}',
        '{"cpv": "app-misc/lib", "repo": "test_repo"}',
    ],
)
def test_a_malformed_request_is_refused(line):
    with pytest.raises(ValueError):
        worker.parse_request(line)


def test_parse_request():
    assert worker.parse_request(request("app-misc/lib-1")) == worker.Request(
        "app-misc/lib-1", "test_repo"
    )


def test_a_request_that_cannot_be_tried_is_an_error(machine):
    events, status, stderr = machine.worker(
        "not json",
        request("app-misc/missing-1"),
        request("app-misc/lib-1", repo="no_repo"),
        request("app-misc/lib-1"),
    )
    assert status == 0, stderr
    assert [sorted(e) for e in events[:3]] == [["error"]] * 3
    assert events[-1] == {"merged": "app-misc/lib-1"}


def test_the_worker_takes_no_entries():
    assert cli.main(["--worker", "app-misc/lib-1"]) == cli.EXIT_USAGE
