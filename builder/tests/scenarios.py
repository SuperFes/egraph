"""Throwaway installed systems, as ResolverPlayground keyword arguments.

Every comparative test runs against every scenario, so a case added here is
checked against portage by every query at once.
"""

# Installed versions of dev-libs/v in the atoms scenario.
ATOM_VERSIONS = (
    "0.9",
    "1",
    "1.0",
    "1.0-r1",
    "1.0.0",
    "1.00",
    "1.01",
    "1.02",
    "1.1",
    "1.10",
    "1.0a",
    "1.0_alpha",
    "1.0_beta2",
    "1.0_pre",
    "1.0_rc1",
    "1.0_p1",
    "1.0_p1_alpha",
    "2.0-r2",
    "10",
    "20260101",
)

SCENARIOS = {
    # The fork's _InstalledGraph test system.
    "reference": {
        "installed": {
            "app-misc/root-1": {
                "EAPI": "8",
                "IUSE": "flag",
                "USE": "flag",
                "RDEPEND": "dev-libs/lib:1 || ( dev-libs/alt-a dev-libs/alt-b )",
                "DEPEND": "flag? ( dev-libs/cond ) !flag? ( dev-libs/nocond )",
                "REQUIRES": "x86_64: libz.so.1",
            },
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": ">=app-misc/root-1"},
            "app-misc/old-1": {"EAPI": "8"},
            "app-misc/bad-1": {"EAPI": "8", "RDEPEND": "|| ( dev-libs/lib"},
            "dev-libs/lib-1": {
                "EAPI": "8",
                "SLOT": "1/1.2",
                "RDEPEND": "!app-misc/old",
            },
            "dev-libs/lib-2": {"EAPI": "8", "SLOT": "2"},
            "dev-libs/alt-a-1": {"EAPI": "8"},
            "dev-libs/cond-1": {"EAPI": "8"},
            "dev-libs/nocond-1": {"EAPI": "8"},
            "sys-libs/zlib-1": {"EAPI": "8", "PROVIDES": "x86_64: libz.so.1"},
        },
        "world": ["app-misc/root", "app-misc/user"],
    },
    # USE dependencies, including (+)/(-) defaults for flags missing from IUSE.
    "use-deps": {
        "world": ["app-misc/a"],
        "installed": {
            "dev-libs/lib-1": {"EAPI": "8", "IUSE": "ssl +gtk", "USE": "ssl"},
            "app-misc/a-1": {
                "EAPI": "8",
                "IUSE": "ssl gtk",
                "USE": "ssl",
                "RDEPEND": " ".join(
                    (
                        "dev-libs/lib[ssl]",
                        "dev-libs/lib[gtk]",
                        "dev-libs/lib[-gtk]",
                        "dev-libs/lib[qt(+)]",
                        "dev-libs/lib[qt(-)]",
                        "dev-libs/lib[-qt(-)]",
                        "dev-libs/lib[ssl?]",
                        "dev-libs/lib[gtk?]",
                        "dev-libs/lib[ssl=]",
                        "dev-libs/lib[gtk=]",
                        "dev-libs/lib[!gtk?]",
                    )
                ),
            },
        },
    },
    # Slots, sub-slots and the slot operators as the vdb records them after a merge.
    "slots": {
        "world": ["app-misc/a"],
        "installed": {
            "dev-libs/lib-1.2": {"EAPI": "8", "SLOT": "1/1.2"},
            "dev-libs/lib-2.0": {"EAPI": "8", "SLOT": "2/2.0"},
            "app-misc/a-1": {
                "EAPI": "8",
                "RDEPEND": " ".join(
                    (
                        "dev-libs/lib:1",
                        "dev-libs/lib:2/2.0=",
                        "dev-libs/lib:1/1.1=",
                        "dev-libs/lib:*",
                        ">=dev-libs/lib-2",
                        "=dev-libs/lib-1*",
                        "~dev-libs/lib-2.0",
                        "dev-libs/lib:3",
                    )
                ),
            },
        },
    },
    # || groups: nested, with all-of alternatives, and with nothing satisfied.
    "any-of": {
        "world": ["app-misc/x", "app-misc/y", "app-misc/z"],
        "installed": {
            "dev-libs/a-1": {"EAPI": "8"},
            "dev-libs/b-1": {"EAPI": "8"},
            "dev-libs/c-1": {"EAPI": "8"},
            "app-misc/x-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( ( dev-libs/a dev-libs/missing ) dev-libs/b )",
                "DEPEND": "|| ( dev-libs/gone || ( dev-libs/c dev-libs/a ) )",
                "BDEPEND": "|| ( dev-libs/nothing-a dev-libs/nothing-b )",
                "PDEPEND": "dev-libs/absent !!dev-libs/c",
            },
            # A || group emptied by USE: never satisfied from EAPI 7, dropped before.
            "app-misc/y-1": {
                "EAPI": "8",
                "IUSE": "u",
                "DEPEND": "|| ( u? ( dev-libs/a ) )",
            },
            "app-misc/z-1": {
                "EAPI": "6",
                "IUSE": "u",
                "DEPEND": "|| ( u? ( dev-libs/a ) )",
            },
        },
    },
    # Atom matching: every version shape portage orders, sub-slots, and USE dependencies with
    # defaults against both kinds of implicit IUSE (EAPI 4 patterns, EAPI 8 IUSE_EFFECTIVE).
    # No world file, which depclean refuses to work without.
    "atoms": {
        "user_config": {
            "make.conf": [
                'IUSE_IMPLICIT="prefix"',
                'USE_EXPAND="ELIBC KERNEL"',
                'USE_EXPAND_HIDDEN="ELIBC KERNEL"',
                'USE_EXPAND_IMPLICIT="ARCH ELIBC"',
                'USE_EXPAND_UNPREFIXED="ARCH"',
                'USE_EXPAND_VALUES_ARCH="x86 amd64"',
                'USE_EXPAND_VALUES_ELIBC="glibc musl"',
            ],
        },
        "installed": {
            **{
                f"dev-libs/v-{version}": {"EAPI": "8", "SLOT": str(slot)}
                for slot, version in enumerate(ATOM_VERSIONS)
            },
            "app-misc/s-1": {"EAPI": "8", "SLOT": "1/1.5"},
            "app-misc/s-2": {"EAPI": "8", "SLOT": "2/2.0"},
            "app-misc/u4-1": {"EAPI": "4", "IUSE": "a +b", "USE": "a elibc_glibc x86"},
            "app-misc/u8-1": {
                "EAPI": "8",
                "IUSE": "a +b",
                "USE": "a elibc_glibc x86 amd64 stray",
            },
        },
    },
    # Root sets and what depclean keeps: world with a nested user set, @system and @profile
    # from the profile, several slots under one atom, || choices, and a cycle nothing needs.
    "roots": {
        "repo_configs": {
            "test_repo": {"layout.conf": ["profile-formats = profile-set"]}
        },
        "profile": {"packages": ["*sys-apps/base", "app-misc/prof"]},
        "sets": {"myset": ["app-misc/set-member"]},
        "world": ["app-misc/world", "sys-kernel/sources"],
        "world_sets": ["@myset"],
        "installed": {
            "app-misc/world-1": {
                "EAPI": "8",
                "RDEPEND": "dev-libs/a virtual/v dev-libs/impl-b",
                "DEPEND": "dev-libs/build-only",
                "BDEPEND": "dev-util/tool",
            },
            "dev-libs/a-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( dev-libs/alt-x dev-libs/alt-y )",
            },
            "dev-libs/alt-x-1": {"EAPI": "8"},
            "dev-libs/alt-y-1": {"EAPI": "8"},
            "virtual/v-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( dev-libs/impl-a dev-libs/impl-b )",
            },
            "dev-libs/impl-a-1": {"EAPI": "8"},
            "dev-libs/impl-b-1": {"EAPI": "8"},
            "dev-libs/build-only-1": {"EAPI": "8"},
            "dev-util/tool-1": {"EAPI": "8"},
            "sys-apps/base-1": {"EAPI": "8", "RDEPEND": "sys-libs/core"},
            "sys-libs/core-1": {"EAPI": "8"},
            "app-misc/prof-1": {"EAPI": "8"},
            "app-misc/set-member-1": {"EAPI": "8"},
            "sys-kernel/sources-1": {"EAPI": "8", "SLOT": "1"},
            "sys-kernel/sources-2": {"EAPI": "8", "SLOT": "2"},
            "app-misc/orphan-1": {"EAPI": "8", "RDEPEND": "dev-libs/orphan-dep"},
            "dev-libs/orphan-dep-1": {"EAPI": "8"},
            "dev-libs/cycle-a-1": {"EAPI": "8", "RDEPEND": "dev-libs/cycle-b"},
            "dev-libs/cycle-b-1": {"EAPI": "8", "RDEPEND": "dev-libs/cycle-a"},
        },
    },
    # An installed package whose ebuild has since dropped a dependency. depclean re-reads it from
    # the ebuild (--dynamic-deps, on by default); egraph keeps what the vdb recorded.
    "dynamic-deps": {
        "world": ["app-misc/dyn"],
        "ebuilds": {"app-misc/dyn-1": {"EAPI": "8", "KEYWORDS": "x86"}},
        "installed": {
            "app-misc/dyn-1": {"EAPI": "8", "RDEPEND": "dev-libs/dep"},
            "dev-libs/dep-1": {"EAPI": "8"},
        },
    },
    # Sonames across multilib categories.
    "sonames": {
        "world": ["app-misc/tool", "app-misc/nocategory"],
        "installed": {
            "sys-libs/zlib-1": {
                "EAPI": "8",
                "PROVIDES": "x86_32: libz.so.1 x86_64: libz.so.1",
            },
            "dev-libs/openssl-3": {
                "EAPI": "8",
                "PROVIDES": "x86_64: libssl.so.3 libcrypto.so.3",
                "REQUIRES": "x86_64: libz.so.1",
            },
            "app-misc/tool-1": {
                "EAPI": "8",
                "REQUIRES": "x86_32: libz.so.1 x86_64: libssl.so.3 libgone.so.1",
            },
            "app-misc/nocategory-1": {"EAPI": "8", "REQUIRES": "libz.so.1"},
        },
    },
}
