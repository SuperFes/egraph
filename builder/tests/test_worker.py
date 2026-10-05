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
from test_actions import BUILDER_DIR, PORTAGE_LIB, merge_environment, portage_program

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
pkg_postrm() {
	mkdir -p "${EROOT}"/var/lib
	echo "${PF} removed $(usev doc) for ${REPLACED_BY_VERSION:-nothing}" >> "${EROOT}"/var/lib/postrm
}
"""

# Both install the same file: one blocks the other, and takes it over.
SHARED = """S="${WORKDIR}"
src_install() {
	echo "${PN}" > "${T}"/data
	insinto /usr/share/shared
	doins "${T}"/data
	insinto /usr/share/${PN}
	doins "${T}"/data
}
pkg_postrm() {
	mkdir -p "${EROOT}"/var/lib
	echo "${PF} removed" >> "${EROOT}"/var/lib/postrm
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
    "app-misc/old-1": {**PLAIN, "MISC_CONTENT": SHARED},
    "app-misc/new-1": {**PLAIN, "RDEPEND": "!app-misc/old", "MISC_CONTENT": SHARED},
    # Each phase appends its name as it runs.
    "app-misc/phases-1": {
        **PLAIN,
        "MISC_CONTENT": """S="${WORKDIR}"
mark() { mkdir -p "${EROOT}"/var/tmp; echo "$1" >> "${EROOT}"/var/tmp/marks; }
pkg_pretend() { mark pretend; }
pkg_setup() { mark setup; }
src_unpack() { mark unpack; }
src_compile() { mark compile; }
src_install() { mark install; }
""",
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
        self.environment.update(CLEAN_DELAY="0", EMERGE_WARNING_DELAY="0")
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
        # As the user runs it, with no egraph-build to import.
        environment = dict(self.environment, PYTHONPATH=PORTAGE_LIB)
        result = subprocess.run(
            f"{portage_program('emerge')} --ask=n --color=n --nospinner "
            + " ".join(args),
            shell=True,
            env=environment,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        return result

    def worker(self, *lines, options=()):
        """The worker's events for lines, each request's in turn, its exit status and what it
        wrote to stderr."""
        result = subprocess.run(
            [sys.executable, "-m", "egraph_build", "--worker", *options],
            input="".join(line + "\n" for line in lines),
            env=self.environment,
            capture_output=True,
            text=True,
        )
        events = [json.loads(line) for line in result.stdout.splitlines()]
        return events, result.returncode, result.stderr

    def talk(self):
        """A worker to send requests to one at a time, reading its events as they come."""
        return subprocess.Popen(
            [sys.executable, "-m", "egraph_build", "--worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=self.environment,
            text=True,
        )

    def builddir(self, cpv):
        return os.path.join(self.playground.settings["PORTAGE_TMPDIR"], "portage", cpv)

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


def request(cpv, repo="test_repo", **fields):
    return json.dumps({"cpv": cpv, "repo": repo, **fields})


def build(cpv, repo="test_repo"):
    return json.dumps({"build": cpv, "repo": repo})


def merge(cpv, **fields):
    return json.dumps({"merge": cpv, **fields})


def uninstall(cpv, **fields):
    return json.dumps({"uninstall": cpv, **fields})


def merged(cpv):
    """A merge's events."""
    return [{"phase": phase} for phase in worker.PHASES + ("merge",)] + [
        {"merged": cpv}
    ]


def built_then_merged(cpv):
    """A build's events, then its merge's."""
    return [{"phase": phase} for phase in worker.PHASES] + [
        {"built": cpv},
        {"phase": "merge"},
        {"merged": cpv},
    ]


def locked(path):
    """Whether another process holds portage's lock on path."""
    from portage.exception import TryAgain
    from portage.locks import lockfile, unlockfile

    # Let go, the category directory goes once empty.
    if not os.path.isdir(os.path.dirname(path)):
        return False
    try:
        lock = lockfile(path, wantnewlockfile=True, flags=os.O_NONBLOCK)
    except TryAgain:
        return True
    unlockfile(lock)
    return False


def uninstalled(cpv):
    return [{"phase": "unmerge"}, {"uninstalled": cpv}]


@pytest.fixture
def machine(playground, tmp_path):
    return Machine(playground, tmp_path)


def same_as_emerge(machine, before, emerges, lines):
    """After the emerges before: the emerges, and the lines given one worker, leave the same
    system; the worker's events."""
    for args in before:
        machine.emerge(args)
    # The user's configuration file, which each version's waits beside.
    os.makedirs(machine.path("etc"), exist_ok=True)
    with open(machine.path("etc", "files.conf"), "a") as f:
        f.write("the user's\n")
    machine.save()
    for args in emerges:
        machine.emerge(args)
    emerged = machine.state()
    machine.restore()
    events, status, stderr = machine.worker(*lines)
    assert status == 0, stderr
    worked = machine.state()
    assert worked == emerged, differences(emerged, worked)
    return events


def merged_as_emerge(machine, before, targets):
    """After the cpvs before, merged by emerge -1: the targets merged one by one by emerge -1,
    and all by one worker, leave the same system; the worker's events for them."""
    return same_as_emerge(
        machine,
        [f"-1 ={cpv}" for cpv in before],
        [f"-1 ={cpv}" for cpv in targets],
        map(request, targets),
    )


def holds(machine, facts):
    """Each path's content, or None where nothing is."""
    for path, content in facts.items():
        if content is None:
            assert not os.path.lexists(machine.path(path)), path
        else:
            with open(machine.path(path)) as f:
                assert f.read() == content, path


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
                "var/lib/postrm": "files-1 removed doc for 2\n",
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
    assert events == [e for cpv in targets for e in merged(cpv)]
    holds(machine, facts)


def test_a_merge_takes_files_over_from_what_it_blocks(machine):
    """emerge merges new, which blocks old, before it uninstalls old: given old as a blocker,
    the merge may install the file both have, which then leaves old's CONTENTS, so the uninstall
    keeps it."""
    events = same_as_emerge(
        machine,
        ["-1 =app-misc/old-1"],
        ["-1 =app-misc/new-1"],
        [
            request("app-misc/new-1", blockers=["app-misc/old-1"]),
            uninstall("app-misc/old-1"),
        ],
    )
    assert events == merged("app-misc/new-1") + uninstalled("app-misc/old-1")
    holds(
        machine,
        {
            "usr/share/shared/data": "new\n",
            "usr/share/new/data": "new\n",
            "usr/share/old": None,
            "var/db/pkg/app-misc/old-1": None,
            "var/lib/postrm": "old-1 removed\n",
        },
    )


def test_a_build_then_its_merge_leave_what_emerge_does(machine):
    events = same_as_emerge(
        machine,
        ["-1 =app-misc/files-1"],
        ["-1 =app-misc/files-2"],
        [build("app-misc/files-2"), merge("app-misc/files-2")],
    )
    assert events == built_then_merged("app-misc/files-2")


def until_final(process):
    """The events of the request just sent, up to the one ending it."""
    events = []
    while True:
        events.append(json.loads(process.stdout.readline()))
        if "phase" not in events[-1]:
            return events


def test_the_build_directory_stays_locked_from_the_build_to_its_merge(machine):
    """As emerge keeps it while a built package waits its turn to merge."""
    builddir = machine.builddir("app-misc/lib-1")
    process = machine.talk()
    try:
        process.stdin.write(build("app-misc/lib-1") + "\n")
        process.stdin.flush()
        assert until_final(process)[-1] == {"built": "app-misc/lib-1"}
        assert locked(builddir)
        assert not machine.installed("app-misc/lib-1")
        process.stdin.write(merge("app-misc/lib-1") + "\n")
        process.stdin.flush()
        assert until_final(process)[-1] == {"merged": "app-misc/lib-1"}
        assert not locked(builddir)
        assert machine.installed("app-misc/lib-1")
    finally:
        process.stdin.close()
        status = process.wait()
        process.stdout.close()
    assert status == 0


def test_a_merge_needs_its_build_and_a_worker_keeps_several_built(machine):
    """Built packages wait for their merges in any order, as emerge's merge-wait keeps them; a
    package already built waits for its merge before building again."""
    files = machine.builddir("app-misc/files-1")
    events, status, stderr = machine.worker(
        merge("app-misc/lib-1"),
        build("app-misc/lib-1"),
        build("app-misc/files-1"),
        build("app-misc/lib-1"),
        merge("app-misc/lib-1"),
        merge("app-misc/files-1"),
    )
    assert status == 0, stderr
    errors = [e for e in events if "error" in e]
    assert errors == [
        {"error": "app-misc/lib-1 is not built here"},
        {"error": "app-misc/lib-1 is built and waits for its merge"},
    ]
    assert events[-1] == {"merged": "app-misc/files-1"}
    assert machine.installed("app-misc/lib-1")
    assert machine.installed("app-misc/files-1")
    assert not locked(files)


def test_a_merge_keeps_what_others_recorded_in_the_mtimedb(machine):
    """Workers beside each other each merge into the mtimedb as the others left it."""
    mtimedb = machine.path("var/cache/edb/mtimedb")
    # A library directory for env-update to record.
    os.makedirs(machine.path("usr/lib64"), exist_ok=True)
    process = machine.talk()
    try:
        process.stdin.write(build("app-misc/lib-1") + "\n")
        process.stdin.flush()
        assert until_final(process)[-1] == {"built": "app-misc/lib-1"}
        recorded = {}
        if os.path.exists(mtimedb):
            with open(mtimedb) as f:
                recorded = json.load(f)
        recorded["updates"] = {"elsewhere": 1}
        os.makedirs(os.path.dirname(mtimedb), exist_ok=True)
        with open(mtimedb, "w") as f:
            json.dump(recorded, f)
        process.stdin.write(merge("app-misc/lib-1") + "\n")
        process.stdin.flush()
        assert until_final(process)[-1] == {"merged": "app-misc/lib-1"}
    finally:
        process.stdin.close()
        process.wait()
        process.stdout.close()
    with open(mtimedb) as f:
        kept = json.load(f)
    assert kept["updates"] == {"elsewhere": 1}
    assert machine.path("usr/lib64") in kept["ldpath"]


def test_a_worker_runs_on_a_copy_of_portage_while_a_new_one_merges(machine, tmp_path):
    """--copy-portage copies the running portage, and a worker given it imports portage and
    runs the ebuilds from there, as emerge runs from its copy while it updates itself.
    """
    copy = tmp_path / "portage-copy"
    assert cli.main(["--copy-portage", "--output", str(copy)]) == cli.EXIT_OK
    for path in ("bin/ebuild.sh", "lib/portage/__init__.py", "lib/_emerge/__init__.py"):
        assert (copy / path).exists(), path
    assert cli.main(["--copy-portage", "--output", str(copy)]) == cli.EXIT_FAILURE
    # The copy's ebuild.sh marks each phase it runs.
    marks = tmp_path / "marks"
    ebuild_sh = copy / "bin" / "ebuild.sh"
    text = ebuild_sh.read_text()
    first, rest = text.split("\n", 1)
    ebuild_sh.write_text(f'{first}\necho "${{EBUILD_PHASE}}" >> "{marks}"\n{rest}')
    events, status, stderr = machine.worker(
        request("app-misc/lib-1"), options=["--portage-copy", str(copy)]
    )
    assert status == 0, stderr
    assert events[-1] == {"merged": "app-misc/lib-1"}, stderr
    assert "compile" in marks.read_text().split()


@pytest.mark.parametrize("background", [False, True])
def test_in_the_background_build_output_goes_only_to_the_log(machine, background):
    """As emerge's does with more than one job."""
    events, status, stderr = machine.worker(
        build("app-misc/broken-1"), options=["--background"] if background else []
    )
    assert status == 0, stderr
    assert events[-1]["failed"] == "compile"
    with open(events[-1]["log"]) as f:
        assert ">>> Compiling source" in f.read()
    assert (">>> Compiling source" in stderr) != background


def test_each_phase_runs_once_as_in_emerge(machine):
    marks = machine.path("var/tmp/marks")
    machine.save()
    machine.emerge("-1 =app-misc/phases-1")
    with open(marks) as f:
        emerged = f.read().split()
    machine.restore()
    events, status, stderr = machine.worker(request("app-misc/phases-1"))
    assert status == 0, stderr
    with open(marks) as f:
        assert f.read().split() == emerged
    assert emerged == ["pretend", "setup", "unpack", "compile", "install"]


def test_a_failed_build_lets_its_directory_go(machine):
    events, status, stderr = machine.worker(build("app-misc/broken-1"))
    assert status == 0, stderr
    assert events[-1]["failed"] == "compile"
    assert not locked(machine.builddir("app-misc/broken-1"))


def test_without_its_blockers_a_merge_cannot_take_their_files(machine):
    """FEATURES=protect-owned refuses the file old installed."""
    machine.emerge("-1 =app-misc/old-1")
    events, status, stderr = machine.worker(request("app-misc/new-1"))
    assert status == 0, stderr
    assert events[:-1] == merged("app-misc/new-1")[:-1]
    assert events[-1]["failed"] == "merge"
    assert not machine.installed("app-misc/new-1")
    holds(machine, {"usr/share/shared/data": "old\n"})


@pytest.mark.parametrize("selected", [False, True], ids=["oneshot", "selected"])
def test_an_uninstall_is_emerge_C(machine, selected):
    """As emerge -C uninstalls it, its atom dropped from the world file."""
    events = same_as_emerge(
        machine,
        [("" if selected else "-1 ") + "=app-misc/files-1"],
        ["-C =app-misc/files-1"],
        [uninstall("app-misc/files-1", clean_world=True)],
    )
    assert events == uninstalled("app-misc/files-1")
    holds(
        machine,
        {
            "usr/share/files": None,
            "var/lib/portage/world": "",
            "var/lib/postrm": "files-1 removed doc for nothing\n",
        },
    )


def test_an_uninstall_keeps_the_world_file_unless_asked(machine):
    machine.emerge("=app-misc/files-1")
    events, status, stderr = machine.worker(uninstall("app-misc/files-1"))
    assert status == 0, stderr
    assert events == uninstalled("app-misc/files-1")
    holds(machine, {"var/lib/portage/world": "app-misc/files\n"})


def test_a_merge_records_its_world_atom(machine):
    """As emerge records its argument once merged."""
    events = same_as_emerge(
        machine,
        [],
        ["=app-misc/lib-1"],
        [request("app-misc/lib-1", world="app-misc/lib")],
    )
    assert events == merged("app-misc/lib-1")
    holds(machine, {"var/lib/portage/world": "app-misc/lib\n"})


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
        request("app-misc/lib-1", blockers="app-misc/old-1"),
        request("app-misc/lib-1", blockers=[1]),
        request("app-misc/lib-1", blockers=["app-misc/old"]),
        request("app-misc/lib-1", world=["app-misc/lib"]),
        uninstall(1),
        uninstall("app-misc/lib"),
        uninstall("app-misc/lib-1", clean_world="yes"),
        json.dumps({"uninstall": "app-misc/lib-1", "cpv": "app-misc/lib-1"}),
        json.dumps({"build": "app-misc/lib-1"}),
        build("app-misc/lib"),
        merge("app-misc/lib"),
        json.dumps({"merge": "app-misc/lib-1", "build": "app-misc/lib-1", "repo": "r"}),
    ],
)
def test_a_malformed_request_is_refused(line):
    with pytest.raises(ValueError):
        worker.parse_request(line)


def test_parse_request():
    assert worker.parse_request(request("app-misc/lib-1")) == worker.Merge(
        "app-misc/lib-1", "test_repo"
    )
    assert worker.parse_request(
        request("app-misc/new-1", blockers=["app-misc/old-1"], world="app-misc/new")
    ) == worker.Merge(
        "app-misc/new-1", "test_repo", ("app-misc/old-1",), "app-misc/new"
    )
    assert worker.parse_request(uninstall("app-misc/old-1")) == worker.Uninstall(
        "app-misc/old-1"
    )
    assert worker.parse_request(
        uninstall("app-misc/old-1", clean_world=True)
    ) == worker.Uninstall("app-misc/old-1", True)
    assert worker.parse_request(build("app-misc/lib-1")) == worker.Build(
        "app-misc/lib-1", "test_repo"
    )
    assert worker.parse_request(
        merge("app-misc/new-1", blockers=["app-misc/old-1"], world="app-misc/new")
    ) == worker.MergeBuilt("app-misc/new-1", ("app-misc/old-1",), "app-misc/new")


def test_a_request_that_cannot_be_tried_is_an_error(machine):
    events, status, stderr = machine.worker(
        "not json",
        request("app-misc/missing-1"),
        request("app-misc/lib-1", repo="no_repo"),
        uninstall("app-misc/files-1"),
        request("app-misc/lib-1"),
    )
    assert status == 0, stderr
    assert [sorted(e) for e in events[:4]] == [["error"]] * 4
    assert events[-1] == {"merged": "app-misc/lib-1"}


def test_the_worker_takes_no_entries():
    assert cli.main(["--worker", "app-misc/lib-1"]) == cli.EXIT_USAGE


def test_ebuilds_inherit_no_pythonpath_of_the_builders():
    builder = "/src/egraph/builder"
    assert worker.without_builder(f"{builder}:/usr/lib/portage", builder) == (
        "/usr/lib/portage"
    )
    assert worker.without_builder(builder + "/", builder) is None
    assert worker.without_builder("", builder) is None
    assert worker.without_builder("/a::/b", builder) == "/a:/b"
