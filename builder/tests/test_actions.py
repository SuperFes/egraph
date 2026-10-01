"""update and install: the plan shown, verified, confirmed, and merged by the real emerge."""

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
    "app-misc/a-2": {"EAPI": "8", "KEYWORDS": "x86"},
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
        self.builder = script(
            tmp_path / "egraph-build",
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


@pytest.fixture
def system(gnupg_home, tmp_path):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    made = []

    def make(pretend=None, removable=False):
        playground = ResolverPlayground(
            ebuilds={**EBUILDS, **REMOVABLE},
            installed={**INSTALLED, **REMOVABLE} if removable else INSTALLED,
            world=(
                ["app-misc/a", "app-misc/user", "app-misc/leaf"]
                if removable
                else ["app-misc/a"]
            ),
            user_config={"make.conf": ('EMERGE_DEFAULT_OPTS="--jobs 2 --ask --deep"',)},
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
