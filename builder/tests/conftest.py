import grp
import importlib
import os
import pwd
import shutil
import subprocess
import sys
import tempfile
from typing import NamedTuple

import pytest

import portage
import portage.tests
from portage.util._eventloop.global_event_loop import global_event_loop

from scenarios import SCENARIOS


def playground_arguments(name):
    """A scenario's ResolverPlayground arguments.

    Installed packages get the playground's accepted keyword unless they set their own, so they
    are unmasked: depclean passes over a masked installed package without a visible ebuild, and
    most scenario packages have none.
    """
    arguments = dict(SCENARIOS[name])
    arguments.pop("updates", None)
    arguments.pop("bounded", None)
    arguments.pop("unrequested", None)
    arguments.pop("pulls", None)
    arguments.pop("held", None)
    arguments.pop("files", None)
    arguments["installed"] = {
        cpv: {"KEYWORDS": "x86", **metadata}
        for cpv, metadata in arguments.get("installed", {}).items()
    }
    return arguments


def make_playground(name, removed=(), deselected=()):
    """A scenario's playground, with the package moves and files ResolverPlayground cannot write
    itself; without the installed cpvs removed and the world atoms deselected."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    arguments = playground_arguments(name)
    arguments["installed"] = {
        cpv: metadata
        for cpv, metadata in arguments["installed"].items()
        if cpv.split("::")[0] not in removed
    }
    if "world" in arguments:
        arguments["world"] = [
            atom for atom in arguments["world"] if atom not in deselected
        ]
    playground = ResolverPlayground(**arguments)
    updates = SCENARIOS[name].get("updates", {})
    if updates:
        portdb = playground.trees[playground.eroot]["porttree"].dbapi
        path = os.path.join(
            portdb.getRepositoryPath("test_repo"), "profiles", "updates"
        )
        os.makedirs(path, exist_ok=True)
        for quarter, lines in updates.items():
            with open(os.path.join(path, quarter), "w") as f:
                f.writelines(f"{line}\n" for line in lines)
    files = SCENARIOS[name].get("files", {})
    if files:
        profile = os.path.realpath(
            os.path.join(playground.eroot, "etc", "portage", "make.profile")
        )
        for name_, lines in files.items():
            if name_.startswith("profile/"):
                path = os.path.join(profile, name_[len("profile/") :])
            else:
                path = os.path.join(playground.eroot, name_)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.writelines(f"{line}\n" for line in lines)
        # The files ResolverPlayground cannot write itself, read as a fresh config would.
        playground.settings, playground.trees = playground._load_config()
    return playground


class System(NamedTuple):
    eroot: str
    vardb: object
    # The playground's trees by EROOT, with the root_config emerge would use.
    trees: object


def pytest_report_header(config):
    return f"portage {portage.VERSION} from {os.path.dirname(portage.__file__)}"


# Settings portage takes from the environment over make.conf.
PORTAGE_OVERRIDES = (
    "ACCEPT_KEYWORDS",
    "ACCEPT_LICENSE",
    "EMERGE_DEFAULT_OPTS",
    "FEATURES",
    "MAKEOPTS",
    "PORTAGE_BINHOST",
    "PORTAGE_TMPDIR",
    "USE",
)


@pytest.fixture(autouse=True, scope="session")
def portage_environment():
    """The process state portage's own test suite sets up before touching portage."""
    # Pretend the current user is the portage user so playgrounds need no privileges.
    os.environ["PORTAGE_USERNAME"] = pwd.getpwuid(os.getuid()).pw_name
    os.environ["PORTAGE_GRPNAME"] = grp.getgrgid(os.getgid()).gr_name
    if "portage.data" in sys.modules:
        importlib.reload(portage.data)
    portage._internal_caller = True
    # The tests choose egraph's store, builder and layout, whatever the user's environment says.
    for variable in (
        "EGRAPH_STORE",
        "EGRAPH_BUILD",
        "EGRAPH_EMERGE",
        "EGRAPH_LAYOUT",
        "EGRAPH_GLYPHS",
    ):
        os.environ.pop(variable, None)
    # Nor the host's replace-slots list, which egraph reads under / without --config-root.
    os.environ["EGRAPH_REPLACE_SLOTS"] = os.devnull
    # The playgrounds' make.conf decides how portage behaves, not the caller's environment,
    # which portage lets override it.
    for variable in PORTAGE_OVERRIDES:
        os.environ.pop(variable, None)
    # Never read the running system's config by accident.
    portage._disable_legacy_globals()
    yield
    global_event_loop().close()


@pytest.fixture(scope="session")
def gnupg_home():
    """ResolverPlayground signs binpkgs, so it needs the test keys from a portage checkout.

    An installed portage has the playground but not the keys; EGRAPH_TEST_KEYS names a
    checkout's, so that any portage can be tested.
    """
    keys = os.environ.get("EGRAPH_TEST_KEYS") or os.path.join(
        os.path.dirname(portage.tests.__file__), ".gnupg"
    )
    if not os.path.isdir(keys):
        pytest.skip(
            f"no test GPG keys at {keys}: set EGRAPH_TEST_KEYS to a portage checkout's"
            " lib/portage/tests/.gnupg"
        )
    home = tempfile.mkdtemp(prefix="egraph-gpg-")
    shutil.copytree(keys, home, dirs_exist_ok=True)
    os.chmod(home, 0o700)
    previous = os.environ.get("PORTAGE_GNUPGHOME")
    os.environ["PORTAGE_GNUPGHOME"] = home
    yield home
    if previous is None:
        del os.environ["PORTAGE_GNUPGHOME"]
    else:
        os.environ["PORTAGE_GNUPGHOME"] = previous
    # Signing daemonizes a gpg-agent that would outlive the session.
    subprocess.run(
        ["gpgconf", "--homedir", home, "--kill", "all"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    shutil.rmtree(home, ignore_errors=True)


@pytest.fixture(scope="session")
def playgrounds(gnupg_home):
    """One throwaway system per scenario name, built on first use."""

    built = {}

    def get(name):
        if name not in built:
            built[name] = make_playground(name)
        playground = built[name]
        trees = playground.trees
        return System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)

    yield get
    for playground in built.values():
        playground.cleanup()


@pytest.fixture
def mutable_playground(gnupg_home):
    """A throwaway system of its own, for tests that change it."""

    made = []

    def make(name):
        playground = make_playground(name)
        made.append(playground)
        return playground

    yield make
    for playground in made:
        playground.cleanup()


def portdb(system):
    return system.trees[system.eroot]["porttree"].dbapi


def write_stores(system, path, request_all=False, eroot="/"):
    """The installed store at path and the evaluated one beside it, as one builder run writes
    them; with request_all, as if every repository cp had been evaluated on request. They
    record eroot as the root they describe."""
    from egraph_build import evaluated, installed, store
    from egraph_build.profile import implicit_iuse

    meta = store.Meta("0", "0", eroot, 0, implicit_iuse(system.vardb.settings))
    store.write(path, store.encode(installed.build(system.vardb), meta))
    requested = portdb(system).cp_all() if request_all else ()
    layer, _ = evaluated.rebuild(
        system.vardb, portdb(system), None, None, requested=requested
    )
    meta = store.EvaluatedMeta("0", "0", eroot, 0, 0)
    store.write(store.evaluated_path(path), store.encode_evaluated(layer, meta))


def write_index(system, path, eroot="/"):
    """The repository index beside the installed store at path."""
    from egraph_build import repository, store

    meta = store.RepositoryMeta("0", "0", eroot, 0)
    index = repository.read(portdb(system))
    store.write(store.repository_path(path), store.encode_repository(index, meta))


def notice_lines(system, path, tmp_path):
    """egraph notices as lines, on the stores at path (not refreshed), egraph-build reading the
    rest of system."""
    builder = tmp_path / "egraph-build"
    builder_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    portage_lib = os.path.dirname(os.path.dirname(portage.__file__))
    builder.write_text(
        "#!/bin/sh\n"
        f'PYTHONPATH="{builder_dir}:{portage_lib}" exec "{sys.executable}" -m egraph_build "$@"\n'
    )
    builder.chmod(0o755)
    result = subprocess.run(
        [os.environ["EGRAPH"], "--store", str(path), "--no-refresh"]
        + ["--config-root", system.eroot, "--builder", str(builder), "notices"],
        capture_output=True,
        text=True,
        env=system.vardb.settings.environ(),
    )
    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    return result.stdout.splitlines()


def fake_vartree(system):
    """emerge's own view of the installed packages under --dynamic-deps=y (test code only)."""
    from _emerge.FakeVartree import FakeVartree

    fake = FakeVartree(system.trees[system.eroot]["root_config"], dynamic_deps=True)
    fake.sync()
    return fake


@pytest.fixture(params=[True, False], ids=["dynamic-deps", "vdb-deps"])
def dynamic_deps(request):
    """emerge's --dynamic-deps, on as by default or off."""
    return request.param


def dynamic_option(dynamic_deps):
    return ("--dynamic-deps", "y" if dynamic_deps else "n")


@pytest.fixture(params=sorted(SCENARIOS))
def scenario(request, playgrounds):
    return playgrounds(request.param)
