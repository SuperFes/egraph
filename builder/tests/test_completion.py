"""The generated completion scripts, loaded by the shells themselves."""

import os
import re
import shlex
import shutil
import subprocess
import time
from pathlib import Path

import pytest
from conftest import write_stores

COMPLETIONS = os.environ.get("EGRAPH_COMPLETIONS")
EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not COMPLETIONS or not EGRAPH,
    reason="set EGRAPH_COMPLETIONS to the directory with the scripts and EGRAPH to egraph",
)


def needs(shell):
    if not shutil.which(shell):
        pytest.skip(f"needs {shell}")


@pytest.fixture
def env(playgrounds, tmp_path):
    """What a shell completes in: egraph first in PATH, its stores the repository scenario's."""
    store = tmp_path / "installed.egraph"
    write_stores(playgrounds("repository"), store)
    path = os.path.dirname(os.path.abspath(EGRAPH)) + os.pathsep + os.environ["PATH"]
    return {"PATH": path, "EGRAPH_STORE": str(store), "HOME": str(tmp_path)}


def bash(words, env):
    """COMPREPLY for the line of words, split at = and : as COMP_WORDBREAKS splits it."""
    script = Path(COMPLETIONS) / "egraph.bash"
    line = " ".join(words)
    split = [part for word in words for part in re.split(r"([=:]+)", word) if part]
    if words[-1] == "":
        split.append("")
    quoted = " ".join(shlex.quote(word) for word in split)
    result = subprocess.run(
        [
            "bash",
            "--norc",
            "--noprofile",
            "-c",
            f"source {shlex.quote(str(script))}; COMP_WORDS=({quoted}); "
            f"COMP_CWORD=$((${{#COMP_WORDS[@]}} - 1)); COMP_LINE={shlex.quote(line)}; "
            "COMP_POINT=${#COMP_LINE}; _egraph 2>/dev/null; "
            'printf "%s\\n" "${COMPREPLY[@]}"',
        ],
        capture_output=True,
        text=True,
        check=True,
        env=env,
    )
    return sorted(line for line in result.stdout.splitlines() if line)


def fish(line, env):
    script = Path(COMPLETIONS) / "egraph.fish"
    result = subprocess.run(
        [
            "fish",
            "--no-config",
            "-c",
            f"source {shlex.quote(str(script))}; complete -C {shlex.quote(line)}",
        ],
        capture_output=True,
        text=True,
        check=True,
        env=env,
    )
    return sorted(line.split("\t")[0] for line in result.stdout.splitlines() if line)


def test_bash_completes_commands_options_values_and_packages(env):
    needs("bash")
    assert "updates" in bash(["egraph", "up"], env)
    assert bash(["egraph", "--la"], env) == ["--layout"]
    assert bash(["egraph", "--layout", "h"], env) == ["human"]
    # A global option's value is not the command.
    assert bash(["egraph", "--store", "x", "wh"], env) == ["why"]
    assert bash(["egraph", "updates", "--t"], env) == ["--table", "--tree"]
    assert "--root" not in bash(["egraph", "updates", "--"], env)
    assert bash(["egraph", "why", "--with-bdeps", ""], env) == ["n", "y"]
    assert bash(["egraph", "why", ""], env) == ["app-misc/", "dev-libs/"]
    assert bash(["egraph", "rdeps", "dev-libs/"], env) == [
        "dev-libs/lib",
        "dev-libs/new",
        "dev-libs/old",
    ]
    assert bash(["egraph", "rdeps", "dev-libs/lib-"], env) == [
        "dev-libs/lib-1",
        "dev-libs/lib-2",
    ]
    assert bash(["egraph", "soname", ""], env) == []
    # Packages only the repositories have, and names without their category.
    assert bash(["egraph", "install", "www-apps/"], env) == [
        "www-apps/helper",
        "www-apps/unused",
    ]
    assert bash(["egraph", "install", "unu"], env) == ["unused"]
    assert bash(["egraph", "install", "@w"], env) == ["@world"]
    assert bash(["egraph", "sync", ""], env) == ["overlay", "test_repo"]
    # bash replaces only what follows the = or :, its own word.
    assert bash(["egraph", "install", "=dev-libs/lib-2"], env) == [
        "dev-libs/lib-2",
        "dev-libs/lib-2.1",
    ]
    assert bash(["egraph", "install", "dev-libs/lib:"], env) == [":1", ":2"]


def test_fish_completes_commands_options_values_and_packages(env):
    needs("fish")
    assert "updates" in fish("egraph up", env)
    assert fish("egraph --layout h", env) == ["human"]
    assert fish("egraph --store x wh", env) == ["why"]
    assert fish("egraph updates --t", env) == ["--table", "--tree"]
    assert "--root" not in fish("egraph updates --", env)
    assert fish("egraph why --with-bdeps ", env) == ["n", "y"]
    assert fish("egraph why ", env) == ["app-misc/", "dev-libs/"]
    assert fish("egraph soname ", env) == []
    assert fish("egraph install www-apps/u", env) == ["www-apps/unused"]
    assert fish("egraph install =dev-libs/lib-2", env) == [
        "=dev-libs/lib-2",
        "=dev-libs/lib-2.1",
    ]
    assert fish("egraph install dev-libs/lib:", env) == [
        "dev-libs/lib:1",
        "dev-libs/lib:2",
    ]
    assert fish("egraph sync o", env) == ["overlay"]


def test_zsh_completes_commands_options_values_and_packages(env, tmp_path):
    needs("zsh")
    if not shutil.which("tmux"):
        pytest.skip("needs tmux")
    socket = f"egraph-zsh-{os.getpid()}"
    fpath = tmp_path / "functions"
    fpath.mkdir()
    shutil.copy(Path(COMPLETIONS) / "_egraph", fpath)

    def tmux(*args):
        return subprocess.run(
            ["tmux", "-L", socket, "-f", "/dev/null", *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def screen_after(keys, text):
        tmux("send-keys", "-t", "t", "C-u")
        tmux("send-keys", "-t", "t", "-l", keys)
        tmux("send-keys", "-t", "t", "Tab")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            screen = tmux("capture-pane", "-p", "-t", "t").stdout
            if text in screen:
                return screen
            time.sleep(0.1)
        raise AssertionError(f"{text!r} never appeared:\n{screen}")

    variables = " ".join(f"{name}={shlex.quote(value)}" for name, value in env.items())
    tmux(
        "new-session",
        "-d",
        "-s",
        "t",
        "-x",
        "120",
        "-y",
        "30",
        f"env -i TERM=xterm {variables} zsh -f -i",
    )
    try:
        tmux(
            "send-keys",
            "-t",
            "t",
            "-l",
            f"PS1='%% '; fpath=({shlex.quote(str(fpath))} $fpath); "
            "autoload -Uz compinit; compinit -u -D; echo RE''ADY\n",
        )
        screen_after("", "READY")
        tmux("send-keys", "-t", "t", "C-l")
        screen_after("egraph orp", "egraph orphans")
        screen_after("egraph --layout h", "egraph --layout human")
        screen = screen_after("egraph updates --t", "--tree")
        assert "--table" in screen
        screen_after("egraph why dev-libs/o", "egraph why dev-libs/old")
        screen_after("egraph install www-apps/u", "egraph install www-apps/unused")
        screen_after("egraph install dev-libs/lib:", "dev-libs/lib:2")
    finally:
        tmux("kill-server")
