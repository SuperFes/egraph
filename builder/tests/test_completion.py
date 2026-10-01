"""The generated completion scripts, loaded by the shells themselves."""

import os
import shlex
import shutil
import subprocess
import time
from pathlib import Path

import pytest

COMPLETIONS = os.environ.get("EGRAPH_COMPLETIONS")

pytestmark = pytest.mark.skipif(
    not COMPLETIONS, reason="set EGRAPH_COMPLETIONS to the directory with the scripts"
)


def needs(shell):
    if not shutil.which(shell):
        pytest.skip(f"needs {shell}")


@pytest.fixture
def root(tmp_path):
    """A root whose vdb holds a few packages, versions of every shape among them."""
    for cpv in [
        "dev-libs/openssl-3.4.0",
        "dev-libs/openssl-3.5.0-r1",
        "app-misc/foo-bar-1.0_rc2_p3",
        "media-fonts/font-adobe-100dpi-1.0.4",
    ]:
        (tmp_path / "var/db/pkg" / cpv).mkdir(parents=True)
    return tmp_path


def bash(words, root):
    script = Path(COMPLETIONS) / "egraph.bash"
    words = " ".join(shlex.quote(word) for word in words)
    result = subprocess.run(
        [
            "bash",
            "--norc",
            "--noprofile",
            "-c",
            f"source {shlex.quote(str(script))}; COMP_WORDS=({words}); "
            f"COMP_CWORD=$((${{#COMP_WORDS[@]}} - 1)); _egraph 2>/dev/null; "
            'printf "%s\\n" "${COMPREPLY[@]}"',
        ],
        capture_output=True,
        text=True,
        check=True,
        env={"PATH": os.environ["PATH"], "ROOT": str(root)},
    )
    return sorted(line for line in result.stdout.splitlines() if line)


def fish(line, root):
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
        env={"PATH": os.environ["PATH"], "ROOT": str(root), "HOME": str(root)},
    )
    return sorted(line.split("\t")[0] for line in result.stdout.splitlines() if line)


def test_bash_completes_commands_options_values_and_packages(root):
    needs("bash")
    assert "updates" in bash(["egraph", "up"], root)
    assert bash(["egraph", "--la"], root) == ["--layout"]
    assert bash(["egraph", "--layout", "h"], root) == ["human"]
    # A global option's value is not the command.
    assert bash(["egraph", "--store", "x", "wh"], root) == ["why"]
    assert bash(["egraph", "updates", "--t"], root) == ["--table", "--tree"]
    assert "--root" not in bash(["egraph", "updates", "--"], root)
    assert bash(["egraph", "why", "--with-bdeps", ""], root) == ["n", "y"]
    assert bash(["egraph", "why", ""], root) == [
        "app-misc/foo-bar",
        "dev-libs/openssl",
        "media-fonts/font-adobe-100dpi",
    ]
    assert bash(["egraph", "rdeps", "dev-libs/o"], root) == ["dev-libs/openssl"]
    assert bash(["egraph", "soname", ""], root) == []


def test_fish_completes_commands_options_values_and_packages(root):
    needs("fish")
    assert "updates" in fish("egraph up", root)
    assert fish("egraph --layout h", root) == ["human"]
    assert fish("egraph --store x wh", root) == ["why"]
    assert fish("egraph updates --t", root) == ["--table", "--tree"]
    assert "--root" not in fish("egraph updates --", root)
    assert fish("egraph why --with-bdeps ", root) == ["n", "y"]
    assert fish("egraph why ", root) == [
        "app-misc/foo-bar",
        "dev-libs/openssl",
        "media-fonts/font-adobe-100dpi",
    ]
    assert fish("egraph soname ", root) == []


def test_zsh_completes_commands_options_values_and_packages(root, tmp_path):
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

    env = f"env -i PATH={shlex.quote(os.environ['PATH'])} TERM=xterm ROOT={shlex.quote(str(root))}"
    tmux("new-session", "-d", "-s", "t", "-x", "120", "-y", "30", f"{env} zsh -f -i")
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
        screen_after("egraph why dev-libs/o", "egraph why dev-libs/openssl")
        screen_after("egraph why media-fonts/f", "media-fonts/font-adobe-100dpi")
    finally:
        tmux("kill-server")
