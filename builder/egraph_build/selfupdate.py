"""A copy of the running portage for the workers to run from while a plan merges a new one, as
emerge's _prepare_self_update copies its own: its bin directory and its Python packages.
"""

import os
import shutil
import sys


def copy_portage(target):
    """Copies the running portage to target, a directory that does not exist yet."""
    import portage
    from portage.const import PORTAGE_PYM_PACKAGES

    bin_path = os.path.join(target, "bin")
    lib = os.path.join(target, "lib")
    os.makedirs(target)
    shutil.copytree(portage._bin_path, bin_path, symlinks=True)
    os.mkdir(lib)
    for package in PORTAGE_PYM_PACKAGES:
        shutil.copytree(
            os.path.join(portage._pym_path, package),
            os.path.join(lib, package),
            symlinks=True,
        )
    for path in (target, bin_path, lib):
        os.chmod(path, 0o755)


def use_copy(target):
    """Runs this process, and the ebuilds it starts, on the copy at target: called before
    anything imports portage."""
    if "portage" in sys.modules:
        raise RuntimeError("portage is imported already")
    sys.path.insert(0, os.path.join(target, "lib"))
    import portage

    # Where its config takes PORTAGE_BIN_PATH and PORTAGE_PYM_PATH from.
    portage._bin_path = os.path.join(target, "bin")
    portage._pym_path = os.path.join(target, "lib")
