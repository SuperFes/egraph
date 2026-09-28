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
    are visible: depclean prefers visible packages, and egraph assumes every installed package
    is (it has no masking information yet).
    """
    arguments = dict(SCENARIOS[name])
    arguments["installed"] = {
        cpv: {"KEYWORDS": "x86", **metadata}
        for cpv, metadata in arguments.get("installed", {}).items()
    }
    return arguments


class System(NamedTuple):
    eroot: str
    vardb: object
    # The playground's trees by EROOT, with the root_config emerge would use.
    trees: object


def pytest_report_header(config):
    return f"portage {portage.VERSION} from {os.path.dirname(portage.__file__)}"


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
    for variable in ("EGRAPH_STORE", "EGRAPH_BUILD", "EGRAPH_LAYOUT", "EGRAPH_GLYPHS"):
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
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    built = {}

    def get(name):
        if name not in built:
            playground = ResolverPlayground(**playground_arguments(name))
            built[name] = playground
        playground = built[name]
        trees = playground.trees
        return System(playground.eroot, trees[playground.eroot]["vartree"].dbapi, trees)

    yield get
    for playground in built.values():
        playground.cleanup()


@pytest.fixture
def mutable_playground(gnupg_home):
    """A throwaway system of its own, for tests that change it."""
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    made = []

    def make(name):
        playground = ResolverPlayground(**playground_arguments(name))
        made.append(playground)
        return playground

    yield make
    for playground in made:
        playground.cleanup()


@pytest.fixture(params=sorted(SCENARIOS))
def scenario(request, playgrounds):
    return playgrounds(request.param)
