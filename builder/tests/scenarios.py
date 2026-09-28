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
    # The repository side: ebuilds that changed under installed packages, versions masked by
    # keyword, package.mask and license, a package that moved, and a USE default that changed.
    "repository": {
        "world": ["app-misc/dyn", "app-misc/slotop", "app-misc/gone", "app-misc/flags"],
        "ebuilds": {
            # The dependency changed without a revision bump.
            "app-misc/dyn-1": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "RDEPEND": "dev-libs/new",
            },
            "dev-libs/new-1": {"EAPI": "8", "KEYWORDS": "x86"},
            # Built against lib:1; the ebuild's := keeps what it was built with.
            "app-misc/slotop-1": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "RDEPEND": "dev-libs/lib:=",
            },
            "dev-libs/lib-1": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "1/1"},
            "dev-libs/lib-2": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "2/2"},
            "dev-libs/lib-2.1": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "2/2.1"},
            # A newer version, but only on the testing keyword.
            "app-misc/testing-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/testing-2": {"EAPI": "8", "KEYWORDS": "~x86"},
            # The installed version is now in package.mask.
            "app-misc/masked-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/masked-2": {"EAPI": "8", "KEYWORDS": "x86"},
            # A license nobody accepted.
            "app-misc/eula-1": {"EAPI": "8", "KEYWORDS": "x86", "LICENSE": "EULA"},
            "app-misc/newname-1": {"EAPI": "8", "KEYWORDS": "x86"},
            # new is on by default now; old was on when it was built.
            "app-misc/flags-1": {"EAPI": "8", "KEYWORDS": "x86", "IUSE": "+new old"},
            # A second repository: one package only there, and one version in both.
            "app-misc/over-1::overlay": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/over-2::overlay": {"EAPI": "8", "KEYWORDS": "x86"},
            "dev-libs/new-1::overlay": {"EAPI": "8", "KEYWORDS": "x86"},
        },
        "installed": {
            "app-misc/dyn-1": {"EAPI": "8", "RDEPEND": "dev-libs/old"},
            "dev-libs/old-1": {"EAPI": "8"},
            "dev-libs/new-1": {"EAPI": "8"},
            "app-misc/slotop-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:1/1="},
            "dev-libs/lib-1": {"EAPI": "8", "SLOT": "1/1"},
            "dev-libs/lib-2": {"EAPI": "8", "SLOT": "2/2"},
            "app-misc/testing-1": {"EAPI": "8"},
            "app-misc/masked-2": {"EAPI": "8"},
            "app-misc/eula-1": {"EAPI": "8", "LICENSE": "EULA"},
            # Gone from the repository, and depending on a package that moved since.
            "app-misc/gone-1": {"EAPI": "8", "RDEPEND": "app-misc/oldname"},
            "app-misc/newname-1": {"EAPI": "8"},
            "app-misc/flags-1": {"EAPI": "8", "IUSE": "+new old", "USE": "old"},
            "app-misc/over-1::overlay": {"EAPI": "8"},
        },
        "user_config": {
            "package.mask": ("=app-misc/masked-2",),
            "make.conf": ('ACCEPT_LICENSE="* -EULA"',),
        },
        # Written to the repository's profiles/updates by the fixtures.
        "updates": {"1Q-2026": ("move app-misc/oldname app-misc/newname",)},
    },
    # Dependencies behind flags the installed build left off: nested, negated, inside and around
    # ||, on a use-masked and a use-forced flag, on an arch flag, behind a contradiction, already
    # depended on, not installed, and one whose USE dependency follows the flag. x86 is implicit,
    # as the arch is in a real profile: the ebuild would be invalid otherwise.
    "possible": {
        "world": ["app-misc/host"],
        "user_config": {
            "make.conf": [
                'USE_EXPAND_IMPLICIT="ARCH"',
                'USE_EXPAND_UNPREFIXED="ARCH"',
                'USE_EXPAND_VALUES_ARCH="x86 amd64"',
            ],
        },
        "ebuilds": {
            "app-misc/host-1": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "IUSE": "a b c doc minimal masked forced",
                "RDEPEND": " ".join(
                    (
                        "dev-libs/always",
                        "a? ( dev-libs/x dev-libs/always )",
                        "a? ( b? ( dev-libs/y ) )",
                        "a? ( b? ( c? ( dev-libs/deep ) ) )",
                        "!minimal? ( dev-libs/z )",
                        "|| ( dev-libs/w a? ( dev-libs/v ) )",
                        "a? ( || ( dev-libs/w2 dev-libs/v2 ) )",
                        "masked? ( dev-libs/m )",
                        "!forced? ( dev-libs/f )",
                        "!x86? ( dev-libs/arch )",
                        "a? ( !a? ( dev-libs/never ) )",
                        "b? ( dev-libs/absent )",
                        "doc? ( dev-libs/lib[doc?] )",
                        "a? ( !dev-libs/blocked )",
                    )
                ),
                "DEPEND": "doc? ( dev-libs/x )",
            },
        },
        "installed": {
            "app-misc/host-1": {
                "EAPI": "8",
                "IUSE": "a b c doc minimal masked forced",
                "USE": "forced minimal x86",
                "RDEPEND": "dev-libs/always dev-libs/w",
            },
            **{
                f"dev-libs/{name}-1": {"EAPI": "8"}
                for name in (
                    "always",
                    "x",
                    "y",
                    "deep",
                    "z",
                    "w",
                    "v",
                    "w2",
                    "v2",
                    "m",
                    "f",
                    "arch",
                    "never",
                    "blocked",
                )
            },
            "dev-libs/lib-1": {"EAPI": "8", "IUSE": "doc", "USE": "doc"},
        },
        "profile": {
            "package.use.mask": ("app-misc/host masked",),
            "package.use.force": ("app-misc/host forced",),
        },
    },
    # Installed packages masked by their own metadata (a keyword no longer accepted): depclean
    # takes one as unavailable unless an ebuild of its version is visible, so a || group passes
    # over it, and among several installed matches an atom selects an unmasked one.
    "masked-installed": {
        "world": ["app-misc/top"],
        "ebuilds": {"dev-libs/stale-1": {"EAPI": "8", "KEYWORDS": "x86"}},
        "installed": {
            "app-misc/top-1": {
                "EAPI": "8",
                "RDEPEND": " ".join(
                    (
                        "|| ( dev-libs/gone dev-libs/kept )",
                        "|| ( dev-libs/stale dev-libs/spare )",
                        "dev-libs/multi",
                    )
                ),
            },
            "dev-libs/gone-1": {"EAPI": "8", "KEYWORDS": "~x86"},
            "dev-libs/kept-1": {"EAPI": "8"},
            "dev-libs/stale-1": {"EAPI": "8", "KEYWORDS": "~x86"},
            "dev-libs/spare-1": {"EAPI": "8"},
            "dev-libs/multi-1": {"EAPI": "8", "SLOT": "1"},
            "dev-libs/multi-2": {"EAPI": "8", "SLOT": "2", "KEYWORDS": "~x86"},
        },
    },
    # What emerge -u @installed would change, with and without --newuse or --changed-use: an
    # upgrade, a revision, newer slots beside installed ones, a testing version, masked installed
    # versions with a newer and with only an older visible one, a newer installed version whose
    # ebuild is gone, an ebuild left in another repository, one version in two repositories,
    # an upgrade that also changes USE, and every way a flag can differ from its installed build.
    "updates": {
        "world": [
            "app-misc/up",
            "app-misc/rev",
            "dev-libs/slotted:1",
            "dev-libs/slotted:2",
            "app-misc/testing",
            "app-misc/past",
            "app-misc/down",
            "app-misc/gone",
            "app-misc/moved",
            "app-misc/twin",
            "app-misc/both",
            "app-misc/use",
            "app-misc/iuse",
        ],
        "ebuilds": {
            "app-misc/up-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/up-2": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/rev-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/rev-1-r1": {"EAPI": "8", "KEYWORDS": "x86"},
            "dev-libs/slotted-1": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "1"},
            "dev-libs/slotted-1.1": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "1"},
            "dev-libs/slotted-2": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "2"},
            "dev-libs/slotted-3": {"EAPI": "8", "KEYWORDS": "x86", "SLOT": "3"},
            "app-misc/testing-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/testing-2": {"EAPI": "8", "KEYWORDS": "~x86"},
            "app-misc/past-2": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/past-3": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/down-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/down-2": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/gone-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/moved-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/twin-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/both-1": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/both-2": {"EAPI": "8", "KEYWORDS": "x86", "IUSE": "+extra"},
            "app-misc/use-1": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "IUSE": "+new_on new_off new_masked +turned_on turned_off +same",
            },
            "app-misc/iuse-1": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "IUSE": "new_off new_masked",
            },
            "app-misc/only-1::overlay": {"EAPI": "8", "KEYWORDS": "x86"},
            "app-misc/twin-1::overlay": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "IUSE": "+extra",
            },
        },
        "installed": {
            "app-misc/up-1": {"EAPI": "8"},
            "app-misc/rev-1": {"EAPI": "8"},
            "dev-libs/slotted-1": {"EAPI": "8", "SLOT": "1"},
            "dev-libs/slotted-2": {"EAPI": "8", "SLOT": "2"},
            "app-misc/testing-1": {"EAPI": "8"},
            "app-misc/past-2": {"EAPI": "8"},
            "app-misc/down-2": {"EAPI": "8"},
            "app-misc/gone-2": {"EAPI": "8"},
            "app-misc/moved-1::overlay": {"EAPI": "8"},
            "app-misc/twin-1": {"EAPI": "8"},
            "app-misc/both-1": {"EAPI": "8"},
            "app-misc/use-1": {
                "EAPI": "8",
                "IUSE": "gone_on gone_off turned_on turned_off same",
                "USE": "gone_on turned_off same",
            },
            "app-misc/iuse-1": {"EAPI": "8", "IUSE": "gone_off"},
        },
        "user_config": {
            "package.mask": ("=app-misc/past-2", "=app-misc/down-2"),
        },
        "profile": {
            "package.use.mask": (
                "app-misc/use new_masked",
                "app-misc/iuse new_masked",
            ),
        },
    },
}
