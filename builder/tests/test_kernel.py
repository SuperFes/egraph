"""--kernel-sources: the kernel source directories installed packages own."""

import json
import os

import pytest

from egraph_build import cli, kernel

INSTALLED = {
    "sys-kernel/sources-1": {"EAPI": "8", "SLOT": "1"},
    "sys-kernel/sources-2": {"EAPI": "8", "SLOT": "2"},
    "app-misc/other-1": {"EAPI": "8"},
}


@pytest.fixture(scope="module")
def system(gnupg_home):
    from portage.tests.resolver.ResolverPlayground import ResolverPlayground

    playground = ResolverPlayground(installed=INSTALLED)
    eprefix = playground.eprefix
    contents = {
        "sys-kernel/sources-1": [
            "dir /usr",
            "dir /usr/src",
            "dir /usr/src/linux-1-gentoo",
            "dir /usr/src/linux-1-gentoo/kernel",
            "obj /usr/src/linux-1-gentoo/Makefile 0 0",
        ],
        "sys-kernel/sources-2": ["dir /usr/src/linux-2-gentoo"],
        # A file named like one, or a directory elsewhere, is no kernel's; one inside a tree owns
        # it too, as portage's getcontents adds the parents.
        "app-misc/other-1": [
            "dir /usr/src/linux-1-gentoo/extra",
            "obj /usr/src/linux-notes 0 0",
            "dir /usr/src/other",
        ],
    }
    for cpv, lines in contents.items():
        path = os.path.join(playground.eroot, "var", "db", "pkg", cpv, "CONTENTS")
        with open(path, "w") as f:
            for line in lines:
                kind, rest = line.split(" ", 1)
                f.write(f"{kind} {eprefix}{rest}\n")
    yield playground
    playground.cleanup()


def expected(system):
    eprefix = system.eprefix
    return {
        "sys-kernel/sources-1": [f"{eprefix}/usr/src/linux-1-gentoo"],
        "sys-kernel/sources-2": [f"{eprefix}/usr/src/linux-2-gentoo"],
        "app-misc/other-1": [f"{eprefix}/usr/src/linux-1-gentoo"],
    }


def test_each_package_names_the_kernel_trees_it_owns(system):
    vartree = system.trees[system.eroot]["vartree"]
    assert kernel.sources(vartree, list(INSTALLED)) == expected(system)
    with pytest.raises(KeyError):
        kernel.sources(vartree, ["sys-kernel/sources-3"])


def test_cli_writes_them_to_a_file(system, tmp_path, capsys):
    output = tmp_path / "kernel.json"
    argv = [
        "--kernel-sources",
        "--output",
        str(output),
        "--config-root",
        system.eroot,
        "--eprefix",
        system.eprefix,
    ]
    assert cli.main([*argv, *INSTALLED]) == cli.EXIT_OK
    assert json.loads(output.read_text()) == expected(system)
    assert cli.main([*argv, "sys-kernel/sources-3"]) == cli.EXIT_USAGE
    assert "sys-kernel/sources-3 is not installed" in capsys.readouterr().err
    assert cli.main(["--kernel-sources", "sys-kernel/sources-1"]) == cli.EXIT_USAGE
