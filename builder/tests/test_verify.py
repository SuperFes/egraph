"""--verify: egraph's plan beside what the real emerge binary prints for the same request."""

import os
import shlex
import shutil
import subprocess

import portage
import portage.const
import pytest

from conftest import write_stores
from egraph_build.cli import EXIT_REFUSED
from scenarios import SCENARIOS

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

# What egraph always asks emerge for besides the request.
PRETEND = [
    "--root",
    "/",
    "--pretend",
    "--verbose",
    "--color=n",
    "--nospinner",
    "--ignore-default-opts",
]


def egraph(path, *args, emerge):
    return subprocess.run(
        [EGRAPH, "--store", str(path), "--no-refresh", "--emerge", emerge, *args],
        capture_output=True,
        text=True,
    )


def script(path, body):
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)
    return str(path)


def real_emerge(system, tmp_path):
    """The emerge of the portage under test, seeing the playground as its system and leaving
    it as it was."""
    program = os.path.join(portage.const.PORTAGE_BIN_PATH, "emerge")
    if not os.path.exists(program):
        program = shutil.which("emerge")
    settings = system.vardb.settings
    environment = {
        "PORTAGE_OVERRIDE_EPREFIX": settings["EPREFIX"],
        "PORTAGE_REPOSITORIES": settings.repositories.config_string(),
        "PYTHONPATH": os.path.dirname(os.path.dirname(portage.__file__)),
    }
    exports = "".join(
        f"export {name}={shlex.quote(value)}\n" for name, value in environment.items()
    )
    # The playgrounds are shared: emerge must not apply package moves to their vdbs, which later
    # tests read as the scenario wrote them.
    return script(
        tmp_path / "emerge",
        f'{exports}exec {shlex.quote(program)} --package-moves=n "$@"\n',
    )


def fake_emerge(tmp_path, printed, status=0):
    """An emerge that records its arguments, one per line, and prints printed."""
    (tmp_path / "printed").write_text(printed)
    return script(
        tmp_path / "emerge",
        f'printf "%s\\n" "$@" > {shlex.quote(str(tmp_path / "arguments"))}\n'
        f'cat {shlex.quote(str(tmp_path / "printed"))}\n'
        f"exit {status}\n",
    )


UPDATE_MODES = {"u": [], "uDN": ["-D", "-N"], "world": ["--world", "-D"]}


def test_the_real_emerge_leaves_the_shared_playground_as_it_was(playgrounds, tmp_path):
    """The repository scenario's package move stays out of its vdb: emerge would apply it there,
    and the tests that run after on the same playground would read another system."""
    system = playgrounds("repository")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    egraph(path, "updates", "--verify", emerge=real_emerge(system, tmp_path))
    assert system.vardb.aux_get("app-misc/gone-1", ["RDEPEND"]) == ["app-misc/oldname"]


@pytest.mark.parametrize("mode", sorted(UPDATE_MODES))
@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_verified_updates_agree_with_emerge(playgrounds, tmp_path, name, mode):
    """emerge -pu @installed (or @world, with -D and -N) merges what updates plans, wherever it
    can resolve the system at all."""
    import update

    system = playgrounds(name)
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    options = UPDATE_MODES[mode]
    expected = update.updates(
        system.trees,
        system.eroot,
        newuse="-N" in options,
        deep="-D" in options,
        target="@world" if "--world" in options else "@installed",
    )
    result = egraph(
        path, "updates", *options, "--verify", emerge=real_emerge(system, tmp_path)
    )
    if expected.success:
        assert result.returncode == 0, result.stderr
    elif (
        expected.blocked
        or expected.unsatisfied
        or expected.unmet
        or expected.use_changes
    ):
        # Refused alike, for the same blockers after the same merge list, for what nothing
        # satisfies, for REQUIRED_USE unmet, or for the same USE changes.
        assert result.returncode == EXIT_REFUSED, result.stderr
    else:
        assert result.returncode == 1
        assert "emerge --pretend failed:" in result.stderr


@pytest.mark.parametrize(
    "flags, target",
    [
        ([], "dev-libs/fresh"),
        (["-u"], "@world"),
        (["-u", "-D", "-N"], "@world"),
        (["-n"], "app-misc/flagged"),
    ],
)
def test_verified_plans_agree_with_emerge(playgrounds, tmp_path, flags, target):
    """A new package's USE, forced flags and USE_EXPAND groups among it, reads the same from
    emerge's list as egraph shows it."""
    system = playgrounds("pulls")
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    result = egraph(
        path, "plan", *flags, "--verify", target, emerge=real_emerge(system, tmp_path)
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize(
    "flags, target, status",
    [
        ([], "app-misc/chain", EXIT_REFUSED),
        ([], "app-misc/puller", 0),
        ([], "app-misc/lastupd", EXIT_REFUSED),
        (["-u", "-D"], "app-misc/broken", EXIT_REFUSED),
        (["-u", "-D"], "@world", 0),
    ],
)
def test_verified_refusals_agree_with_emerge(
    playgrounds, tmp_path, flags, target, status
):
    """emerge refuses, for a dependency nothing satisfies, what egraph refuses, and falls back
    where egraph does."""
    system = playgrounds("refused")
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    result = egraph(
        path,
        "plan",
        *flags,
        "--verify",
        target,
        emerge=real_emerge(system, tmp_path),
    )
    assert result.returncode == status, result.stderr


@pytest.mark.parametrize(
    "flags, target, status",
    [
        ([], "app-misc/req", EXIT_REFUSED),
        ([], "app-misc/reqok", 0),
        ([], "app-misc/reqdep", EXIT_REFUSED),
        ([], "app-misc/reqchoice", EXIT_REFUSED),
        ([], "app-misc/reqcond", EXIT_REFUSED),
        ([], "app-misc/reqold", 0),
        ([], "app-misc/fb", EXIT_REFUSED),
        ([], "app-misc/rev", 0),
        ([], "app-misc/kinds", 0),
        ([], "app-misc/kinds2", EXIT_REFUSED),
        ([], "app-misc/pd", 0),
        (["-u"], "dev-libs/held", EXIT_REFUSED),
        (["-u", "-D"], "@world", EXIT_REFUSED),
    ],
)
def test_verified_required_use_agrees_with_emerge(
    playgrounds, tmp_path, flags, target, status
):
    """emerge refuses, for REQUIRED_USE unmet, the version egraph refuses, wherever it selects
    one."""
    system = playgrounds("required")
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    result = egraph(
        path, "plan", *flags, "--verify", target, emerge=real_emerge(system, tmp_path)
    )
    assert result.returncode == status, result.stderr


@pytest.mark.parametrize(
    "flags, target, status",
    [
        ([], "app-misc/wantgtk", EXIT_REFUSED),
        ([], "app-misc/wantnoqt", EXIT_REFUSED),
        ([], "app-misc/anyof", 0),
        ([], "app-misc/anyof2", EXIT_REFUSED),
        ([], "app-misc/wantx", EXIT_REFUSED),
        ([], "app-misc/wantold", EXIT_REFUSED),
        ([], "dev-libs/lib[gtk]", EXIT_REFUSED),
        (["-u"], "app-misc/user", EXIT_REFUSED),
        (["-u", "-D"], "@world", EXIT_REFUSED),
    ],
)
def test_verified_use_changes_agree_with_emerge(
    playgrounds, tmp_path, flags, target, status
):
    """emerge asks for the USE changes egraph refuses the plan for, written alike."""
    system = playgrounds("usechange")
    path = tmp_path / "installed.egraph"
    write_stores(system, path, request_all=True)
    result = egraph(
        path, "plan", *flags, "--verify", target, emerge=real_emerge(system, tmp_path)
    )
    assert result.returncode == status, result.stderr


def test_agreement_is_said_for_people(playgrounds, tmp_path):
    system = playgrounds("pulls")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    result = egraph(
        path,
        "--layout",
        "human",
        "--color",
        "never",
        "--glyphs",
        "ascii",
        "updates",
        "--verify",
        emerge=real_emerge(system, tmp_path),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.endswith("\n+ emerge --pretend merges the same.\n")


def test_differences_are_listed_and_fail(playgrounds, tmp_path):
    """emerge is asked for the same request, and each merge only one side makes is listed."""
    system = playgrounds("pulls")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    ours = egraph(path, "updates", "-D", emerge="unused").stdout.splitlines()
    assert ours
    emerge = fake_emerge(
        tmp_path,
        "\nThese are the packages that would be merged, in order:\n\n"
        "[ebuild  N     ] app-misc/bogus-1::test_repo  0 KiB\n",
    )
    result = egraph(path, "updates", "-D", "--verify", emerge=emerge)
    assert result.returncode == 5
    assert (tmp_path / "arguments").read_text().splitlines() == [
        *PRETEND,
        "--update",
        "--deep",
        "@installed",
    ]
    assert result.stdout.splitlines() == ours
    lines = result.stderr.splitlines()
    assert lines[0] == "egraph: updates: emerge --pretend merges otherwise:"
    assert "app-misc/bogus-1::test_repo\temerge\tnew" in lines
    only_ours = [line for line in lines[1:] if line.split("\t")[1] == "egraph"]
    assert len(only_ours) == len(ours)

    human = egraph(
        path,
        "--layout",
        "human",
        "--glyphs",
        "ascii",
        "plan",
        "--verify",
        "-n",
        "app-misc/flagged",
        emerge=emerge,
    )
    assert human.returncode == 5
    assert (tmp_path / "arguments").read_text().splitlines() == [
        *PRETEND,
        "--noreplace",
        "app-misc/flagged",
    ]
    assert "\n! emerge --pretend merges otherwise:\n" in human.stdout
    assert "  app-misc/bogus-1::test_repo  only in emerge (new)\n" in human.stdout


def test_a_failed_emerge_says_why(playgrounds, tmp_path):
    system = playgrounds("pulls")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    emerge = fake_emerge(
        tmp_path,
        "".join(f"line {i}\n" for i in range(30)) + "!!! cannot resolve\n",
        status=1,
    )
    result = egraph(path, "updates", "--verify", emerge=emerge)
    assert result.returncode == 1
    assert result.stderr.endswith(
        "egraph: updates: emerge --pretend failed:\n"
        + "".join(f"line {i}\n" for i in range(11, 30))
        + "!!! cannot resolve\n"
    )


def test_replaced_slots_are_left_out_of_what_emerge_is_held_to(playgrounds, tmp_path):
    """A listed world atom's old slot goes once its new one merges, unless something needs it;
    emerge leaves it to its depclean, so --verify passes over it."""
    system = playgrounds("world-slots")
    path = tmp_path / "installed.egraph"
    write_stores(system, path)
    listed = tmp_path / "replace-slots"
    listed.write_text("# old slots\napp-misc/wine\ndev-libs/libv\nsys-kernel/sources\n")
    emerge = real_emerge(system, tmp_path)
    shown = egraph(
        path, "--replace-slots", str(listed), "updates", "-D", "--world", emerge=emerge
    )
    assert shown.returncode == 0, shown.stderr
    uninstalls = [line for line in shown.stdout.splitlines() if "\tuninstall\t" in line]
    # libv-1 stays for needs-1's dev-libs/libv:1.
    assert uninstalls == [
        "app-misc/wine-1\tuninstall\t\t\tapp-misc/wine-2",
        "sys-kernel/sources-1\tuninstall\t\t\tsys-kernel/sources-3",
        "sys-kernel/sources-2\tuninstall\t\t\tsys-kernel/sources-3",
    ]
    verified = egraph(
        path,
        "--replace-slots",
        str(listed),
        "updates",
        "-D",
        "--world",
        "--verify",
        emerge=emerge,
    )
    assert verified.returncode == 0, verified.stdout + verified.stderr
    # Without the list, as emerge: every slot stays.
    kept = egraph(path, "updates", "-D", "--world", emerge=emerge)
    assert "\tuninstall\t" not in kept.stdout
    bad = tmp_path / "bad"
    bad.write_text("app-misc/wine\n!app-misc/wine\n")
    refused = egraph(path, "--replace-slots", str(bad), "updates", emerge=emerge)
    assert refused.returncode == 1
    assert refused.stderr.startswith(f"egraph: updates: {bad}: line 2: ")
