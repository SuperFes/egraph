"""update and install: the plan shown, verified, confirmed, and merged by the real emerge."""

import json
import os
import shlex
import shutil
import subprocess
import sys

import portage
import portage.const
import pytest

from test_build import age, vdb
from test_refresh import on_terminal

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

BUILDER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTAGE_LIB = os.path.dirname(os.path.dirname(portage.__file__))

EBUILDS = {
    "app-misc/a-1": {"EAPI": "8", "KEYWORDS": "x86"},
    # Installs a protected file, which waits as ._cfg where one is already there, and logs.
    "app-misc/a-2": {
        "EAPI": "8",
        "KEYWORDS": "x86",
        "MISC_CONTENT": 'S="${WORKDIR}"\n'
        'src_install() { mkdir -p "${ED}/etc"; echo two > "${ED}/etc/conf"; }\n'
        'pkg_postinst() { elog "Something to know."; ewarn "Careful."; }\n',
    },
    "app-misc/b-1": {"EAPI": "8", "KEYWORDS": "x86"},
    "app-misc/c-1": {"EAPI": "8", "KEYWORDS": "x86"},
    "app-misc/blocker-1": {"EAPI": "8", "KEYWORDS": "x86", "RDEPEND": "!!app-misc/a"},
}
INSTALLED = {"app-misc/a-1": {"EAPI": "8", "KEYWORDS": "x86"}}
# For remove: user needs lib, leaf needs nothing; both selected.
USER = {"EAPI": "8", "KEYWORDS": "x86", "RDEPEND": "app-misc/lib"}
PLAIN = {"EAPI": "8", "KEYWORDS": "x86"}
REMOVABLE = {"app-misc/user-1": USER, "app-misc/lib-1": PLAIN, "app-misc/leaf-1": PLAIN}


def script(path, body):
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)
    return str(path)


class System:
    """A playground real merges can happen in, with an emerge that logs each run's arguments
    and a builder for egraph to refresh its stores with."""

    def __init__(self, playground, tmp_path, pretend=None):
        self.playground = playground
        self.tmp_path = tmp_path
        self.store = tmp_path / "installed.egraph"
        self.runs = tmp_path / "runs"
        settings = playground.settings
        eprefix = settings["EPREFIX"]
        # As portage's own merge tests: chown and chgrp do nothing, the installing user owns.
        fake_bin = os.path.join(eprefix, "bin")
        os.makedirs(fake_bin, exist_ok=True)
        for name in ("chown", "chgrp"):
            os.symlink(
                portage.process.find_binary("true"), os.path.join(fake_bin, name)
            )
        edb = os.path.join(eprefix, "var", "cache", "edb")
        os.makedirs(edb, exist_ok=True)
        with open(os.path.join(edb, "counter"), "w") as f:
            f.write("100")
        environment = {
            "PORTAGE_OVERRIDE_EPREFIX": eprefix,
            "PORTAGE_REPOSITORIES": settings.repositories.config_string(),
            "PYTHONPATH": PORTAGE_LIB,
            "PATH": fake_bin + ":" + os.environ.get("PATH", ""),
            "PORTAGE_PYTHON": sys.executable,
            "PORTAGE_INST_GID": str(os.getgid()),
            "PORTAGE_INST_UID": str(os.getuid()),
            # Sandbox forbids writing bytecode beside an installed portage.
            "PYTHONDONTWRITEBYTECODE": "1",
        }
        exports = "".join(
            f"export {name}={shlex.quote(value)}\n"
            for name, value in environment.items()
        )
        program = os.path.join(portage.const.PORTAGE_BIN_PATH, "emerge")
        # An installed portage keeps emerge in PATH only, behind its own interpreter.
        run = f"{shlex.quote(sys.executable)} {shlex.quote(program)}"
        if not os.path.exists(program):
            run = shlex.quote(shutil.which("emerge"))
        log = shlex.quote(str(self.runs))
        # Each run's arguments on a line of their own.
        body = f'echo "$*" >> {log}\n'
        if pretend is not None:
            body += f'case " $* " in *" --pretend "*) {pretend} ;; esac\n'
        body += f'{exports}exec {run} "$@"\n'
        self.emerge = script(tmp_path / "emerge", body)
        # Under emerge's EPREFIX, as on a real system: the build root (and so the elog
        # summary's place) comes from it.
        self.builder = script(
            tmp_path / "egraph-build",
            f"export PORTAGE_OVERRIDE_EPREFIX={shlex.quote(eprefix)}\n"
            f'PYTHONPATH="{BUILDER_DIR}:{PORTAGE_LIB}" '
            f'exec "{sys.executable}" -m egraph_build "$@"\n',
        )
        age(playground.eroot)

    def command(self, *args):
        return [
            EGRAPH,
            "--store",
            str(self.store),
            "--config-root",
            self.playground.eroot,
            "--eprefix",
            self.playground.eprefix,
            "--builder",
            self.builder,
            "--emerge",
            self.emerge,
            "--color",
            "never",
            *args,
        ]

    def egraph(self, *args):
        return subprocess.run(
            self.command(*args),
            capture_output=True,
            text=True,
            env=dict(os.environ, EGRAPH_STRICT="1"),
        )

    def emerged(self):
        """The arguments of each run that was not --pretend."""
        if not self.runs.exists():
            return []
        lines = self.runs.read_text().splitlines()
        return [line.split() for line in lines if "--pretend" not in line.split()]

    def asked(self):
        return self.runs.exists()

    def installed(self, cpv):
        return os.path.isdir(vdb(self.playground, cpv))

    def world(self):
        path = os.path.join(self.playground.eroot, "var", "lib", "portage", "world")
        with open(path) as f:
            return f.read().split()


NEWS = """Title: Read me
Author: A Developer <dev@example.org>
Posted: 2026-09-01
Revision: 1
News-Item-Format: 2.0

Something to read.
"""


def leave_notices(machine):
    """A configuration file of the user's that a-2 installs over, and a news item for everyone,
    which emerge marks unread once it has merged something. Their notice lines."""
    etc = os.path.join(machine.playground.eroot, "etc")
    os.makedirs(etc, exist_ok=True)
    with open(os.path.join(etc, "conf"), "w") as f:
        f.write("one\n")
    repo = machine.playground.settings.repositories["test_repo"].location
    item = "2026-09-01-read-me"
    os.makedirs(os.path.join(repo, "metadata", "news", item))
    with open(os.path.join(repo, "metadata", "news", item, f"{item}.en.txt"), "w") as f:
        f.write(NEWS)
    return [
        f"{etc}/conf\tconfig\t{etc}/._cfg0000_conf",
        f"{item}\tnews\ttest_repo\tRead me",
    ]


@pytest.fixture
def system(gnupg_home, tmp_path):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    made = []

    def make(pretend=None, removable=False, make_conf=()):
        playground = ResolverPlayground(
            ebuilds={**EBUILDS, **REMOVABLE},
            installed={**INSTALLED, **REMOVABLE} if removable else INSTALLED,
            world=(
                ["app-misc/a", "app-misc/user", "app-misc/leaf"]
                if removable
                else ["app-misc/a"]
            ),
            user_config={
                "make.conf": ('EMERGE_DEFAULT_OPTS="--jobs 2 --ask --deep"', *make_conf)
            },
        )
        made.append(playground)
        return System(playground, tmp_path, pretend)

    yield make
    for playground in made:
        playground.cleanup()


def test_update_merges_through_emerge_and_refreshes_the_stores(system):
    """update --yes: emerge -u --oneshot carries out the verified plan, with only the execution
    options of EMERGE_DEFAULT_OPTS; the stores are current with the merge right after.
    """
    machine = system()
    result = machine.egraph("update", "--yes")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "app-misc/a-2" in result.stdout
    [run] = machine.emerged()
    assert run[run.index("--ignore-default-opts") :] == [
        "--ignore-default-opts",
        "--ask=n",
        "--jobs=2",
        "--update",
        "--oneshot",
        "@installed",
    ]
    assert machine.installed("app-misc/a-2")
    assert not machine.installed("app-misc/a-1")
    again = machine.egraph("--no-refresh", "updates")
    assert again.returncode == 0, again.stderr
    assert "app-misc/a" not in again.stdout


def test_install_selects_its_targets_unless_oneshot(system):
    machine = system()
    result = machine.egraph("install", "--yes", "app-misc/b")
    assert result.returncode == 0, result.stdout + result.stderr
    assert machine.installed("app-misc/b-1")
    assert "app-misc/b" in machine.world()
    assert result.stdout.splitlines()[-1] == "app-misc/b\tselected"
    result = machine.egraph("install", "--yes", "--oneshot", "app-misc/c")
    assert result.returncode == 0, result.stdout + result.stderr
    assert machine.installed("app-misc/c-1")
    assert "app-misc/c" not in machine.world()
    assert machine.emerged()[-1][-2:] == ["--oneshot", "app-misc/c"]


def test_nothing_to_merge_asks_emerge_nothing(system):
    machine = system()
    result = machine.egraph("install", "--yes", "--noreplace", "app-misc/a")
    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert not machine.asked()
    human = machine.egraph("--layout", "human", "install", "-yn", "app-misc/a")
    assert "Nothing to merge." in human.stdout
    assert not machine.asked()


def test_without_a_terminal_an_action_needs_yes(system):
    machine = system()
    result = machine.egraph("update")
    assert result.returncode == 2
    assert "--yes" in result.stderr
    assert not machine.asked()
    assert machine.installed("app-misc/a-1")


def test_a_plan_emerge_would_refuse_is_not_merged(system):
    machine = system()
    result = machine.egraph("install", "--yes", "app-misc/blocker")
    assert result.returncode == 6, result.stdout + result.stderr
    assert "nothing was merged" in result.stderr
    assert not machine.asked()


def test_a_plan_emerge_would_make_otherwise_is_not_merged(system):
    """emerge --pretend listing nothing for the update: the plans differ, emerge never runs."""
    machine = system(pretend="exit 0")
    result = machine.egraph("update", "--yes")
    assert result.returncode == 5, result.stdout + result.stderr
    assert "nothing was merged" in result.stderr
    assert machine.emerged() == []
    assert machine.installed("app-misc/a-1")


@pytest.mark.skipif(os.geteuid() == 0, reason="root writes anywhere")
def test_an_unwritable_database_stops_before_emerge(system):
    machine = system()
    database = vdb(machine.playground)
    os.chmod(database, 0o555)
    try:
        result = machine.egraph("update", "--yes")
    finally:
        os.chmod(database, 0o755)
    assert result.returncode == 1
    assert f"write access to {database}" in result.stderr
    assert not machine.asked()


@pytest.mark.parametrize("answer", ["y", "n"])
def test_on_a_terminal_emerge_runs_on_yes(system, answer):
    machine = system()
    env = dict(os.environ, EGRAPH_STRICT="1")
    status, printed = on_terminal(machine.command("update"), answer, env)
    assert "Have emerge merge this plan? [y/N]" in printed
    if answer == "n":
        assert status == 1, printed
        assert machine.emerged() == []
        assert machine.installed("app-misc/a-1")
        return
    assert status == 0, printed
    assert machine.installed("app-misc/a-2")


def test_remove_removes_through_depclean_and_deselects(system):
    machine = system(removable=True)
    result = machine.egraph("remove", "--yes", "app-misc/leaf")
    assert result.returncode == 0, result.stdout + result.stderr
    [run] = machine.emerged()
    assert run[run.index("--depclean") :] == [
        "--depclean",
        "--ignore-default-opts",
        "--ask=n",
        "--jobs=2",
        "app-misc/leaf",
    ]
    assert not machine.installed("app-misc/leaf-1")
    assert "app-misc/leaf" not in machine.world()
    assert result.stdout.splitlines()[-1] == "app-misc/leaf\tdeselected"
    again = machine.egraph("--no-refresh", "orphans")
    assert again.returncode == 0, again.stderr
    assert again.stdout == ""


def test_what_something_needs_is_kept_and_nothing_is_asked(system):
    machine = system(removable=True)
    result = machine.egraph("--layout", "human", "remove", "--yes", "app-misc/lib")
    assert result.returncode == 0, result.stderr
    assert "needed by app-misc/user-1" in result.stdout
    assert "Nothing to remove." in result.stdout
    assert not machine.asked()
    assert machine.installed("app-misc/lib-1")


def test_removing_a_package_with_what_it_needs(system):
    machine = system(removable=True)
    result = machine.egraph("remove", "--yes", "app-misc/user", "=app-misc/lib-1")
    assert result.returncode == 0, result.stdout + result.stderr
    assert machine.emerged()[-1][-2:] == ["app-misc/user", "=app-misc/lib-1"]
    assert not machine.installed("app-misc/user-1")
    assert not machine.installed("app-misc/lib-1")


def test_a_removal_emerge_would_make_otherwise_is_not_made(system):
    machine = system(pretend="exit 0", removable=True)
    result = machine.egraph("remove", "--yes", "app-misc/leaf")
    assert result.returncode == 5, result.stdout + result.stderr
    assert "nothing was removed" in result.stderr
    assert machine.emerged() == []
    assert machine.installed("app-misc/leaf-1")


def test_removing_what_is_not_installed_fails_before_emerge(system):
    machine = system(removable=True)
    result = machine.egraph("remove", "--yes", "app-misc/b")
    assert result.returncode == 1
    assert "no installed package matches" in result.stderr
    assert not machine.asked()


def test_select_records_an_installed_package_and_says_so(system):
    machine = system(removable=True)
    result = machine.egraph("--layout", "human", "select", "--yes", "app-misc/lib")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "app-misc/lib" in machine.world()
    assert machine.emerged()[-1][-2:] == ["--noreplace", "app-misc/lib"]
    assert "app-misc/lib joined @selected" in result.stdout
    assert not machine.installed("app-misc/b-1")


def test_select_merges_what_is_not_installed(system):
    machine = system()
    result = machine.egraph("select", "--yes", "app-misc/b")
    assert result.returncode == 0, result.stdout + result.stderr
    assert machine.installed("app-misc/b-1")
    assert "app-misc/b" in machine.world()
    assert result.stdout.splitlines()[-1] == "app-misc/b\tselected"


def test_deselect_lists_what_depclean_would_then_remove(system):
    machine = system(removable=True)
    result = machine.egraph("deselect", "--yes", "app-misc/user")
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert lines[:3] == [
        "app-misc/user\tdeselect",
        "app-misc/lib-1\torphan",
        "app-misc/user-1\torphan",
    ]
    assert lines[-1] == "app-misc/user\tdeselected"
    run = machine.emerged()[-1]
    assert run[run.index("--deselect") :] == [
        "--deselect",
        "--ignore-default-opts",
        "--ask=n",
        "--jobs=2",
        "app-misc/user",
    ]
    assert "app-misc/user" not in machine.world()
    assert machine.installed("app-misc/user-1")


def test_deselecting_what_is_not_selected_does_nothing(system):
    machine = system(removable=True)
    result = machine.egraph("--layout", "human", "deselect", "--yes", "app-misc/lib")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Nothing to deselect." in result.stdout
    assert machine.emerged() == []


def test_notices_follow_an_action(system):
    """What emerge leaves for the user after a merge, listed after the action and by notices."""
    machine = system()
    expected = leave_notices(machine)
    assert machine.egraph("notices").stdout == ""
    result = machine.egraph("update", "--yes")
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines()[-2:] == expected
    notices = machine.egraph("notices")
    assert notices.returncode == 0, notices.stderr
    assert notices.stdout.splitlines() == expected
    human = machine.egraph("--layout", "human", "notices")
    assert "Configuration updates (dispatch-conf):" in human.stdout
    assert "  2026-09-01-read-me  Read me" in human.stdout


@pytest.mark.parametrize("answer", ["y", "n"])
def test_on_a_terminal_dispatch_conf_is_offered(system, answer):
    machine = system()
    leave_notices(machine)
    dispatched = machine.tmp_path / "dispatched"
    dispatch = script(machine.tmp_path / "dispatch-conf", f"env > {dispatched}\n")
    env = dict(os.environ, EGRAPH_STRICT="1")
    command = machine.command("--dispatch-conf", dispatch, "update")
    status, printed = on_terminal(command, ["y", answer], env)
    assert status == 0, printed
    assert "Configuration updates (dispatch-conf):" in printed
    assert "Run dispatch-conf now? [y/N]" in printed
    if answer == "n":
        assert not dispatched.exists()
        return
    assert f"PORTAGE_CONFIGROOT={machine.playground.eroot}" in dispatched.read_text()


def test_nothing_needs_attention(system):
    machine = system()
    human = machine.egraph("--layout", "human", "notices")
    assert human.returncode == 0, human.stderr
    assert human.stdout == "Nothing needs attention.\n"


# A library whose soname moves from 1 to 2, and a program linked against it: updating the
# library has emerge preserve libfoo.so.1 for app-misc/bar until bar is rebuilt.
LIBRARY = r"""S="${WORKDIR}"
src_compile() {
	echo 'int foo(void) { return 0; }' > foo.c || die
	${CC:-cc} -shared -fPIC -Wl,-soname,libfoo.so.%(v)s -o libfoo.so.%(v)s foo.c || die
}
src_install() {
	mkdir -p "${ED}/usr/lib" || die
	cp libfoo.so.%(v)s "${ED}/usr/lib/" || die
	ln -s libfoo.so.%(v)s "${ED}/usr/lib/libfoo.so" || die
}
"""
PROGRAM = r"""S="${WORKDIR}"
src_compile() {
	echo 'int foo(void); int main(void) { return foo(); }' > bar.c || die
	${CC:-cc} -o bar bar.c -L"${EPREFIX}/usr/lib" -Wl,-rpath,"${EPREFIX}/usr/lib" -lfoo || die
}
src_install() {
	mkdir -p "${ED}/usr/bin" || die
	cp bar "${ED}/usr/bin/" || die
}
"""
PRESERVING = {
    "dev-libs/foo-1": {"EAPI": "8", "MISC_CONTENT": LIBRARY % {"v": "1"}},
    "dev-libs/foo-2": {"EAPI": "8", "MISC_CONTENT": LIBRARY % {"v": "2"}},
    "app-misc/bar-1": {
        "EAPI": "8",
        "DEPEND": "dev-libs/foo",
        "RDEPEND": "dev-libs/foo",
        "MISC_CONTENT": PROGRAM,
    },
}


@pytest.fixture
def preserving(gnupg_home, tmp_path):
    """A system with dev-libs/foo-1 and app-misc/bar merged for real, foo-2 waiting."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    scanelf = shutil.which("scanelf")
    compiler = shutil.which("cc")
    if not scanelf or not compiler:
        pytest.skip("needs scanelf and cc")
    playground = ResolverPlayground(ebuilds=PRESERVING, world=["app-misc/bar"])
    try:
        machine = System(playground, tmp_path)
        # portage's linkage map runs the scanelf under EPREFIX.
        bin_dir = os.path.join(playground.eprefix, "usr", "bin")
        os.makedirs(bin_dir, exist_ok=True)
        os.symlink(scanelf, os.path.join(bin_dir, "scanelf"))
        for args in (["--oneshot", "=dev-libs/foo-1"], ["app-misc/bar"]):
            merged = subprocess.run([machine.emerge, *args], capture_output=True)
            assert merged.returncode == 0, merged.stdout + merged.stderr
        # Only egraph's runs count.
        machine.runs.unlink()
        yield machine
    finally:
        playground.cleanup()


def registry(machine):
    path = os.path.join(
        machine.playground.eroot, "var", "lib", "portage", "preserved_libs_registry"
    )
    with open(path) as f:
        return json.load(f)


def test_preserved_libraries_follow_the_merge_that_preserves_them(preserving):
    machine = preserving
    result = machine.egraph("install", "--yes", "--oneshot", "=dev-libs/foo-2")
    assert result.returncode == 0, result.stdout + result.stderr
    library = os.path.join(machine.playground.eprefix, "usr", "lib", "libfoo.so.1")
    expected = [
        f"{library}\tpreserved\tdev-libs/foo-2\tapp-misc/bar-1",
        "app-misc/bar:0\trebuild",
    ]
    assert result.stdout.splitlines()[-2:] == expected
    # --yes offers no rebuild.
    assert len(machine.emerged()) == 1
    # Preserved libraries alone are something to list.
    assert machine.egraph("notices").stdout.splitlines() == expected
    human = machine.egraph("--layout", "human", "notices")
    assert "Preserved libraries" in human.stdout


def test_preserved_rebuild_is_planned_verified_and_run(preserving):
    machine = preserving
    machine.egraph("install", "--yes", "--oneshot", "=dev-libs/foo-2")
    plan = machine.egraph("plan", "--verify", "@preserved-rebuild")
    assert plan.returncode == 0, plan.stdout + plan.stderr
    assert "app-misc/bar-1" in plan.stdout
    result = machine.egraph("install", "--yes", "--oneshot", "@preserved-rebuild")
    assert result.returncode == 0, result.stdout + result.stderr
    assert machine.emerged()[-1][-2:] == ["--oneshot", "@preserved-rebuild"]
    assert registry(machine) == {}
    assert machine.egraph("notices").stdout == ""


def test_without_preserved_libraries_the_set_is_empty(preserving):
    result = preserving.egraph("plan", "@preserved-rebuild")
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == ""


@pytest.mark.parametrize("answer", ["y", "n"])
def test_on_a_terminal_the_rebuild_is_offered_once(preserving, answer):
    machine = preserving
    env = dict(os.environ, EGRAPH_STRICT="1")
    command = machine.command("install", "--oneshot", "=dev-libs/foo-2")
    status, printed = on_terminal(command, ["y", answer], env)
    assert status == 0, printed
    assert "Preserved libraries (egraph install -1 @preserved-rebuild):" in printed
    assert "Rebuilding what uses the preserved libraries:" in printed
    assert printed.count("Have emerge merge this plan? [y/N]") == 2
    if answer == "n":
        assert len(machine.emerged()) == 1
        assert registry(machine) != {}
        return
    assert machine.emerged()[-1][-2:] == ["--oneshot", "@preserved-rebuild"]
    assert registry(machine) == {}
    # The rebuild's own run lists nothing more to rebuild, and offers nothing.
    assert printed.count("Rebuilding what uses") == 1


ELOG = [
    "app-misc/a-2\telog\tLOG\tpostinst\tSomething to know.",
    "app-misc/a-2\telog\tWARN\tpostinst\tCareful.",
]


def test_the_elog_summary_follows_the_run(system):
    """What save_summary appended while emerge ran, shown by egraph instead of emerge's echo."""
    machine = system()
    result = machine.egraph("update", "--yes")
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert lines[lines.index(ELOG[0]) :][:2] == ELOG
    assert "Messages for package app-misc/a-2" not in result.stdout
    human = machine.egraph("--layout", "human", "install", "--yes", "=app-misc/a-2")
    assert human.returncode == 0, human.stdout + human.stderr
    assert "Messages for app-misc/a-2:\n  postinst (LOG)\n    Something to know.\n" in (
        human.stdout
    )


@pytest.mark.parametrize(
    "elog_system, shown",
    [
        # Nothing egraph could read: emerge's echo stays.
        ("echo", False),
        # The summary misses a class echo shows, so echo stays too.
        ("save_summary:error echo", False),
    ],
)
def test_emerge_echoes_what_egraph_cannot_show(system, elog_system, shown):
    machine = system(make_conf=(f'PORTAGE_ELOG_SYSTEM="{elog_system}"',))
    result = machine.egraph("update", "--yes")
    assert result.returncode == 0, result.stdout + result.stderr
    assert ELOG[0] not in result.stdout.splitlines()
    assert "Messages for package app-misc/a-2" in result.stdout
