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
            "package.use": ["app-misc/u8 a"],
        },
        # The installed versions' own ebuilds, for matching ebuilds with the USE they would get.
        "ebuilds": {
            **{
                f"dev-libs/v-{version}": {"EAPI": "8", "SLOT": str(slot)}
                for slot, version in enumerate(ATOM_VERSIONS)
            },
            "app-misc/s-1": {"EAPI": "8", "SLOT": "1/1.5"},
            "app-misc/s-2": {"EAPI": "8", "SLOT": "2/2.0"},
            "app-misc/u4-1": {"EAPI": "4", "IUSE": "a +b"},
            "app-misc/u8-1": {"EAPI": "8", "IUSE": "a +b"},
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
            # A category nothing installed is in.
            "www-apps/unused-1": {
                "EAPI": "8",
                "KEYWORDS": "x86",
                "RDEPEND": "www-apps/helper",
            },
            "www-apps/helper-1": {"EAPI": "8", "KEYWORDS": "x86"},
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
    # Installed dependents whose atoms reject an update's target: a version bound with a lower
    # version to fall back to, one without, a glob, a sub-slot pinned and one under a slot
    # operator, a USE dependency against a --newuse rebuild, a bound inside ||, a bound only the
    # installed build had, a bound held by a package nothing in world needs, and a bound in the
    # ebuild a slot-operator rebuild would use, which also holds another dependent's update.
    "bounds": {
        # Installed dependents hold updates back, which the builder's targets leave to queries.
        "bounded": True,
        "world": [
            "app-misc/pylint",
            "app-misc/qemu",
            "app-misc/mesa",
            "app-misc/comp",
            "app-misc/kwin",
            "app-misc/keyring",
            "app-misc/either",
            "app-misc/dynamic",
            "app-misc/skin",
            "app-misc/effects",
        ],
        "ebuilds": {
            "app-misc/pylint-1": {"EAPI": "8", "RDEPEND": "<dev-libs/astroid-4.1"},
            "dev-libs/astroid-4.0.4": {"EAPI": "8"},
            "dev-libs/astroid-4.0.5": {"EAPI": "8"},
            "dev-libs/astroid-4.3.2": {"EAPI": "8"},
            "app-misc/qemu-1": {"EAPI": "8", "RDEPEND": "~dev-libs/edk2-202408"},
            "dev-libs/edk2-202408": {"EAPI": "8"},
            "dev-libs/edk2-202608": {"EAPI": "8"},
            "app-misc/mesa-1": {"EAPI": "8", "RDEPEND": "=dev-libs/libclc-22*"},
            "dev-libs/libclc-22.1.8": {"EAPI": "8"},
            "dev-libs/libclc-22.1.9": {"EAPI": "8"},
            "dev-libs/libclc-23.1.2": {"EAPI": "8"},
            "app-misc/comp-1": {"EAPI": "8", "RDEPEND": "dev-libs/pinned:0/3"},
            "dev-libs/pinned-0.3": {"EAPI": "8", "SLOT": "0/3"},
            "dev-libs/pinned-0.4": {"EAPI": "8", "SLOT": "0/4"},
            "app-misc/kwin-1": {"EAPI": "8", "RDEPEND": "dev-libs/bound:="},
            "dev-libs/bound-0.3": {"EAPI": "8", "SLOT": "0/3"},
            "dev-libs/bound-0.4": {"EAPI": "8", "SLOT": "0/4"},
            "app-misc/keyring-1": {"EAPI": "8", "RDEPEND": "dev-libs/gcr[gtk]"},
            "dev-libs/gcr-1": {"EAPI": "8", "IUSE": "gtk"},
            "app-misc/either-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( <dev-libs/alt-2 dev-libs/other )",
            },
            "dev-libs/alt-1": {"EAPI": "8"},
            "dev-libs/alt-2": {"EAPI": "8"},
            "dev-libs/other-1": {"EAPI": "8"},
            "app-misc/dynamic-1": {"EAPI": "8", "RDEPEND": "dev-libs/dyn"},
            "dev-libs/dyn-1": {"EAPI": "8"},
            "dev-libs/dyn-2": {"EAPI": "8"},
            "app-misc/stray-1": {"EAPI": "8", "RDEPEND": "<dev-libs/lone-2"},
            "dev-libs/lone-1": {"EAPI": "8"},
            "dev-libs/lone-2": {"EAPI": "8"},
            "app-misc/skin-1": {
                "EAPI": "8",
                "RDEPEND": "<app-misc/rgb-1 app-misc/rgb:=",
            },
            "app-misc/effects-1": {"EAPI": "8", "RDEPEND": "app-misc/rgb:="},
            "app-misc/effects-2": {"EAPI": "8", "RDEPEND": ">=app-misc/rgb-1:="},
            "app-misc/rgb-1_rc3": {"EAPI": "8", "SLOT": "0/rc3"},
            "app-misc/rgb-1": {"EAPI": "8", "SLOT": "0/1"},
        },
        "installed": {
            "app-misc/pylint-1": {"EAPI": "8", "RDEPEND": "<dev-libs/astroid-4.1"},
            "dev-libs/astroid-4.0.4": {"EAPI": "8"},
            "app-misc/qemu-1": {"EAPI": "8", "RDEPEND": "~dev-libs/edk2-202408"},
            "dev-libs/edk2-202408": {"EAPI": "8"},
            "app-misc/mesa-1": {"EAPI": "8", "RDEPEND": "=dev-libs/libclc-22*"},
            "dev-libs/libclc-22.1.8": {"EAPI": "8"},
            "app-misc/comp-1": {"EAPI": "8", "RDEPEND": "dev-libs/pinned:0/3"},
            "dev-libs/pinned-0.3": {"EAPI": "8", "SLOT": "0/3"},
            "app-misc/kwin-1": {"EAPI": "8", "RDEPEND": "dev-libs/bound:0/3="},
            "dev-libs/bound-0.3": {"EAPI": "8", "SLOT": "0/3"},
            "app-misc/keyring-1": {"EAPI": "8", "RDEPEND": "dev-libs/gcr[gtk]"},
            "dev-libs/gcr-1": {"EAPI": "8", "IUSE": "gtk", "USE": "gtk"},
            "app-misc/either-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( <dev-libs/alt-2 dev-libs/other )",
            },
            "dev-libs/alt-1": {"EAPI": "8"},
            "app-misc/dynamic-1": {"EAPI": "8", "RDEPEND": "<dev-libs/dyn-2"},
            "dev-libs/dyn-1": {"EAPI": "8"},
            "app-misc/stray-1": {"EAPI": "8", "RDEPEND": "<dev-libs/lone-2"},
            "dev-libs/lone-1": {"EAPI": "8"},
            "app-misc/skin-1": {
                "EAPI": "8",
                "RDEPEND": "<app-misc/rgb-1 app-misc/rgb:0/rc3=",
            },
            "app-misc/effects-1": {"EAPI": "8", "RDEPEND": "app-misc/rgb:0/rc3="},
            "app-misc/rgb-1_rc3": {"EAPI": "8", "SLOT": "0/rc3"},
        },
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
    # Update targets whose own dependencies reach what nothing installed provides: a new package
    # and one it needs in turn, a || group with nothing installed, one already satisfied, a new
    # slot, a flag-conditional dependency, a kept package's dynamic dependency, and a target
    # needing an update a dependent holds back.
    "pulls": {
        "pulls": True,
        "world": [
            "app-misc/glibmm",
            "app-misc/choose",
            "app-misc/either",
            "app-misc/slotty",
            "app-misc/grown",
            "app-misc/holder",
            "app-misc/plugin",
            "app-misc/flagged",
        ],
        "ebuilds": {
            "app-misc/glibmm-1": {"EAPI": "8"},
            "app-misc/glibmm-2": {"EAPI": "8", "BDEPEND": "dev-cpp/mm-common"},
            "dev-cpp/mm-common-1": {"EAPI": "8", "RDEPEND": "dev-libs/chain"},
            "dev-cpp/mm-common-1.1": {"EAPI": "8", "RDEPEND": "dev-libs/chain"},
            "dev-libs/chain-1": {"EAPI": "8"},
            "dev-libs/chain-2": {"EAPI": "8", "KEYWORDS": "~x86"},
            "app-misc/choose-1": {"EAPI": "8"},
            "app-misc/choose-2": {
                "EAPI": "8",
                "RDEPEND": "|| ( dev-libs/first dev-libs/second )",
            },
            "dev-libs/first-1": {"EAPI": "8"},
            "dev-libs/second-1": {"EAPI": "8"},
            "app-misc/either-1": {"EAPI": "8"},
            "app-misc/either-2": {
                "EAPI": "8",
                "RDEPEND": "|| ( dev-libs/absent dev-libs/present )",
            },
            "dev-libs/absent-1": {"EAPI": "8"},
            "dev-libs/present-1": {"EAPI": "8"},
            "app-misc/slotty-1": {"EAPI": "8", "RDEPEND": "dev-lang/py:3.13"},
            "app-misc/slotty-2": {"EAPI": "8", "RDEPEND": "dev-lang/py:3.14"},
            "dev-lang/py-3.13.1": {"EAPI": "8", "SLOT": "3.13"},
            "dev-lang/py-3.14.1": {"EAPI": "8", "SLOT": "3.14"},
            "app-misc/grown-1": {"EAPI": "8", "RDEPEND": "dev-libs/fresh"},
            "dev-libs/fresh-1": {
                "EAPI": "8",
                "IUSE": " ".join(
                    (
                        "+on off +a10 +a9 +fixed stuck",
                        "python_targets_py3_13 python_targets_py3_12",
                        "python_targets_py3_11 video_cards_intel",
                    )
                ),
            },
            "app-misc/host-1": {"EAPI": "8"},
            "app-misc/host-2": {"EAPI": "8"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "<app-misc/host-2"},
            "app-misc/plugin-1": {"EAPI": "8", "RDEPEND": "app-misc/host"},
            "app-misc/plugin-2": {"EAPI": "8", "RDEPEND": ">=app-misc/host-2"},
            "app-misc/flagged-1": {"EAPI": "8", "IUSE": "+extra"},
            "app-misc/flagged-2": {
                "EAPI": "8",
                "IUSE": "+extra",
                "RDEPEND": "extra? ( dev-libs/extra ) !extra? ( dev-libs/never )",
            },
            "dev-libs/extra-1": {"EAPI": "8"},
            "dev-libs/never-1": {"EAPI": "8"},
        },
        "installed": {
            "app-misc/glibmm-1": {"EAPI": "8"},
            "app-misc/choose-1": {"EAPI": "8"},
            "app-misc/either-1": {"EAPI": "8"},
            "dev-libs/present-1": {"EAPI": "8"},
            "app-misc/slotty-1": {"EAPI": "8", "RDEPEND": "dev-lang/py:3.13"},
            "dev-lang/py-3.13.1": {"EAPI": "8", "SLOT": "3.13"},
            "app-misc/grown-1": {"EAPI": "8"},
            "app-misc/host-1": {"EAPI": "8"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "<app-misc/host-2"},
            "app-misc/plugin-1": {"EAPI": "8", "RDEPEND": "app-misc/host"},
            "app-misc/flagged-1": {"EAPI": "8", "IUSE": "+extra", "USE": "extra"},
        },
        # A new package's USE as emerge shows it: a group per USE_EXPAND variable but the
        # hidden ones, flags the profile forces or masks in parentheses.
        "user_config": {
            "make.conf": [
                'USE_EXPAND="PYTHON_TARGETS VIDEO_CARDS"',
                'USE_EXPAND_HIDDEN="VIDEO_CARDS"',
                'PYTHON_TARGETS="py3_13"',
                'VIDEO_CARDS="intel"',
            ],
        },
        "profile": {
            "package.use.force": (
                "dev-libs/fresh fixed",
                "dev-libs/fresh python_targets_py3_12",
            ),
            "package.use.mask": ("dev-libs/fresh stuck",),
        },
    },
    # Slot-operator rebuilds: dependents bound to a sub-slot an update replaces, through RDEPEND,
    # DEPEND and PDEPEND, one updated itself, one with no visible ebuild to rebuild from, one
    # whose ebuild gained a dependency, and one with :* that never rebuilds. Beside them, two
    # updates in a cycle, one's run-time dependency against the other's build-time one.
    "slotops": {
        "bounded": True,
        "world": [
            "app-misc/rdep",
            "app-misc/ddep",
            "app-misc/pdep",
            "app-misc/moving",
            "app-misc/gone",
            "app-misc/grown",
            "app-misc/star",
            "app-misc/ring",
            "app-misc/loop",
        ],
        "ebuilds": {
            "dev-libs/lib-1": {"EAPI": "8", "SLOT": "0/1"},
            "dev-libs/lib-2": {"EAPI": "8", "SLOT": "0/2"},
            "dev-libs/lone-1": {"EAPI": "8", "SLOT": "0/1"},
            "dev-libs/lone-1.5": {"EAPI": "8", "SLOT": "0/1"},
            "dev-libs/lone-2": {"EAPI": "8", "SLOT": "0/2"},
            "dev-libs/fresh-1": {"EAPI": "8"},
            "app-misc/rdep-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:="},
            "app-misc/ddep-1": {"EAPI": "8", "DEPEND": "dev-libs/lib:="},
            "app-misc/pdep-1": {"EAPI": "8", "PDEPEND": "dev-libs/lib:="},
            "app-misc/moving-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:="},
            "app-misc/moving-2": {"EAPI": "8", "RDEPEND": "dev-libs/lib:="},
            "app-misc/gone-1": {
                "EAPI": "8",
                "KEYWORDS": "~x86",
                "RDEPEND": "dev-libs/lone:=",
            },
            "app-misc/grown-1": {
                "EAPI": "8",
                "RDEPEND": "dev-libs/lib:= dev-libs/fresh",
            },
            "app-misc/star-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:*"},
            "app-misc/ring-1": {"EAPI": "8"},
            "app-misc/ring-2": {"EAPI": "8", "RDEPEND": "app-misc/loop"},
            "app-misc/loop-1": {"EAPI": "8"},
            "app-misc/loop-2": {"EAPI": "8", "BDEPEND": "app-misc/ring"},
        },
        "installed": {
            "dev-libs/lib-1": {"EAPI": "8", "SLOT": "0/1"},
            "dev-libs/lone-1": {"EAPI": "8", "SLOT": "0/1"},
            "app-misc/rdep-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:0/1="},
            "app-misc/ddep-1": {"EAPI": "8", "DEPEND": "dev-libs/lib:0/1="},
            "app-misc/pdep-1": {"EAPI": "8", "PDEPEND": "dev-libs/lib:0/1="},
            "app-misc/moving-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:0/1="},
            "app-misc/gone-1": {"EAPI": "8", "RDEPEND": "dev-libs/lone:0/1="},
            "app-misc/grown-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:0/1="},
            "app-misc/star-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib:*"},
            "app-misc/ring-1": {"EAPI": "8"},
            "app-misc/loop-1": {"EAPI": "8"},
        },
    },
    # What plain -u leaves to -D: a target needing a newer version of a dependency, a kept one
    # with an update of its own, a target whose sub-slot change needs a dependent outside
    # the root sets rebuilt, one bound by such a dependent, a target needing an update another
    # root holds back, and a new slot of an unslotted root atom.
    "shallow": {
        "bounded": True,
        "world": [
            "app-misc/needs",
            "app-misc/user",
            "dev-libs/solib",
            "app-misc/top",
            "dev-libs/capped",
            "app-misc/wants",
            "app-misc/caps",
            "dev-lang/lang",
            "app-misc/plain",
        ],
        "ebuilds": {
            "app-misc/needs-1": {"EAPI": "8"},
            "app-misc/needs-2": {"EAPI": "8", "RDEPEND": ">=dev-libs/base-2"},
            "dev-libs/base-1": {"EAPI": "8"},
            "dev-libs/base-2": {"EAPI": "8"},
            "dev-libs/base-3": {"EAPI": "8"},
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": "dev-libs/idle"},
            "dev-libs/idle-1": {"EAPI": "8"},
            "dev-libs/idle-2": {"EAPI": "8"},
            "dev-libs/solib-1": {"EAPI": "8", "SLOT": "0/1"},
            "dev-libs/solib-2": {"EAPI": "8", "SLOT": "0/2"},
            "app-misc/consumer-1": {"EAPI": "8", "RDEPEND": "dev-libs/solib:="},
            "app-misc/top-1": {
                "EAPI": "8",
                "RDEPEND": "app-misc/consumer app-misc/limiter",
            },
            "dev-libs/capped-1": {"EAPI": "8"},
            "dev-libs/capped-2": {"EAPI": "8"},
            "app-misc/limiter-1": {"EAPI": "8", "RDEPEND": "<dev-libs/capped-2"},
            "app-misc/wants-1": {"EAPI": "8"},
            "app-misc/wants-2": {"EAPI": "8", "RDEPEND": ">=dev-libs/mid-2"},
            "dev-libs/mid-1": {"EAPI": "8"},
            "dev-libs/mid-2": {"EAPI": "8"},
            "app-misc/caps-1": {"EAPI": "8", "RDEPEND": "<dev-libs/mid-2"},
            "dev-lang/lang-1": {"EAPI": "8", "SLOT": "1"},
            "dev-lang/lang-2": {"EAPI": "8", "SLOT": "2"},
            "app-misc/plain-1": {"EAPI": "8"},
            "app-misc/plain-2": {"EAPI": "8", "RDEPEND": "dev-libs/idle"},
        },
        "installed": {
            "app-misc/needs-1": {"EAPI": "8"},
            "dev-libs/base-1": {"EAPI": "8"},
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": "dev-libs/idle"},
            "dev-libs/idle-1": {"EAPI": "8"},
            "dev-libs/solib-1": {"EAPI": "8", "SLOT": "0/1"},
            "app-misc/consumer-1": {"EAPI": "8", "RDEPEND": "dev-libs/solib:0/1="},
            "app-misc/top-1": {
                "EAPI": "8",
                "RDEPEND": "app-misc/consumer app-misc/limiter",
            },
            "dev-libs/capped-1": {"EAPI": "8"},
            "app-misc/limiter-1": {"EAPI": "8", "RDEPEND": "<dev-libs/capped-2"},
            "app-misc/wants-1": {"EAPI": "8"},
            "dev-libs/mid-1": {"EAPI": "8"},
            "app-misc/caps-1": {"EAPI": "8", "RDEPEND": "<dev-libs/mid-2"},
            "dev-lang/lang-1": {"EAPI": "8", "SLOT": "1"},
            "app-misc/plain-1": {"EAPI": "8"},
        },
    },
    # Blockers emerge resolves: a merge's weak blocker on an installed package nothing needs any
    # more (a renamed package, an orphaned holder of a blocker against a merge, an older slot, a
    # tool only a build blocks, a || alternative another satisfies) uninstalls it; one on a
    # version a merge replaces, or in the merge's own slot, needs nothing. Beside them, blockers
    # between installed packages that no merge touches, a build-time one among them.
    "blockers": {
        "world": [
            "app-misc/user",
            "app-misc/top",
            "dev-libs/s",
            "app-misc/p",
            "app-misc/tb",
            "app-misc/anyuser",
            "app-misc/pair-x",
            "app-misc/pair-y",
            "app-misc/flagged",
        ],
        "ebuilds": {
            "app-misc/old-1": {"EAPI": "8"},
            "app-misc/new-1": {"EAPI": "8", "RDEPEND": "!app-misc/old"},
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": "app-misc/old"},
            "app-misc/user-2": {"EAPI": "8", "RDEPEND": "app-misc/new"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "!app-misc/fresh"},
            "app-misc/fresh-1": {"EAPI": "8"},
            "app-misc/top-1": {"EAPI": "8"},
            "app-misc/top-2": {
                "EAPI": "8",
                "RDEPEND": "app-misc/fresh !app-misc/flagged[foo]",
            },
            "dev-libs/s-1": {"EAPI": "8", "SLOT": "1"},
            "dev-libs/s-1.1": {"EAPI": "8", "SLOT": "1"},
            "dev-libs/s-2": {"EAPI": "8", "SLOT": "2", "RDEPEND": "!dev-libs/s:1"},
            "app-misc/p-1": {"EAPI": "8"},
            "app-misc/p-2": {"EAPI": "8", "RDEPEND": "!<app-misc/p-2"},
            "app-misc/tool-1": {"EAPI": "8"},
            "app-misc/tb-1": {"EAPI": "8", "DEPEND": "!app-misc/tool"},
            "app-misc/tb-2": {"EAPI": "8", "DEPEND": "!app-misc/tool"},
            "app-misc/anyuser-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( app-misc/alt-y app-misc/alt-x )",
            },
            "app-misc/anyuser-2": {
                "EAPI": "8",
                "RDEPEND": "|| ( app-misc/alt-y app-misc/alt-x ) !app-misc/alt-x",
            },
            "app-misc/alt-x-1": {"EAPI": "8"},
            "app-misc/alt-y-1": {"EAPI": "8"},
            "app-misc/pair-x-1": {
                "EAPI": "8",
                "RDEPEND": "!app-misc/pair-y !!dev-libs/gone",
            },
            "app-misc/pair-y-1": {"EAPI": "8"},
            "app-misc/flagged-1": {"EAPI": "8", "IUSE": "foo"},
        },
        "installed": {
            "app-misc/old-1": {"EAPI": "8"},
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": "app-misc/old"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "!app-misc/fresh"},
            "app-misc/top-1": {"EAPI": "8"},
            "dev-libs/s-1": {"EAPI": "8", "SLOT": "1"},
            "app-misc/p-1": {"EAPI": "8"},
            "app-misc/tool-1": {"EAPI": "8"},
            "app-misc/tb-1": {"EAPI": "8", "DEPEND": "!app-misc/tool"},
            "app-misc/anyuser-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( app-misc/alt-y app-misc/alt-x )",
            },
            "app-misc/alt-x-1": {"EAPI": "8"},
            "app-misc/alt-y-1": {"EAPI": "8"},
            "app-misc/pair-x-1": {
                "EAPI": "8",
                "RDEPEND": "!app-misc/pair-y !!dev-libs/gone",
            },
            "app-misc/pair-y-1": {"EAPI": "8"},
            "app-misc/flagged-1": {"EAPI": "8", "IUSE": "foo", "USE": ""},
        },
    },
    # Blockers emerge cannot resolve: a weak one on a package @world selects, or a merge needs, or
    # a || takes first; any strong one on a package staying installed, or on the version its own
    # merge replaces; one between two merges; an installed package's against a merge, its holder
    # in @world; and a blocker on a version plain -u keeps, which -uD replaces.
    "blocked": {
        "world": [
            "app-misc/user",
            "app-misc/old",
            "app-misc/suser",
            "app-misc/sholder",
            "app-misc/p",
            "dev-libs/q",
            "app-misc/app",
            "app-misc/pair-x",
            "app-misc/pair-y",
            "app-misc/anyuser",
            "app-misc/needy",
        ],
        "ebuilds": {
            "app-misc/old-1": {"EAPI": "8"},
            "app-misc/new-1": {"EAPI": "8", "RDEPEND": "!app-misc/old"},
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": "app-misc/old"},
            "app-misc/user-2": {"EAPI": "8", "RDEPEND": "app-misc/new"},
            "app-misc/sold-1": {"EAPI": "8"},
            "app-misc/snew-1": {"EAPI": "8", "RDEPEND": "!!app-misc/sold"},
            "app-misc/suser-1": {"EAPI": "8", "RDEPEND": "app-misc/sold"},
            "app-misc/suser-2": {"EAPI": "8", "RDEPEND": "app-misc/snew"},
            "app-misc/sholder-1": {"EAPI": "8", "RDEPEND": "!app-misc/sfresh"},
            "app-misc/sfresh-1": {"EAPI": "8"},
            "app-misc/p-1": {"EAPI": "8"},
            "app-misc/p-2": {"EAPI": "8", "RDEPEND": "!!<app-misc/p-2"},
            "dev-libs/q-1": {"EAPI": "8", "SLOT": "1"},
            "dev-libs/q-2": {"EAPI": "8", "SLOT": "2", "RDEPEND": "!!dev-libs/q:1"},
            "app-misc/a-1": {"EAPI": "8", "RDEPEND": "!app-misc/b"},
            "app-misc/b-1": {"EAPI": "8"},
            "dev-libs/lib-1": {"EAPI": "8"},
            "dev-libs/lib-2": {"EAPI": "8"},
            "app-misc/app-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib"},
            "app-misc/app-2": {"EAPI": "8", "RDEPEND": "!<dev-libs/lib-2 dev-libs/lib"},
            "app-misc/pair-x-1": {"EAPI": "8", "RDEPEND": "!app-misc/pair-y"},
            "app-misc/pair-y-1": {"EAPI": "8"},
            "app-misc/pair-y-2": {"EAPI": "8"},
            "app-misc/anyuser-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( app-misc/alt-x app-misc/alt-y )",
            },
            "app-misc/alt-x-1": {"EAPI": "8"},
            "app-misc/alt-y-1": {"EAPI": "8"},
            "app-misc/anyblock-1": {"EAPI": "8", "RDEPEND": "!app-misc/alt-x"},
            "app-misc/needy-1": {"EAPI": "8"},
            "app-misc/needy-2": {"EAPI": "8", "RDEPEND": "app-misc/anyblock"},
            "app-misc/orph-1": {"EAPI": "8", "RDEPEND": "app-misc/lone"},
            "app-misc/lone-1": {"EAPI": "8"},
            "app-misc/loneblock-1": {"EAPI": "8", "RDEPEND": "!app-misc/lone"},
        },
        "installed": {
            "app-misc/old-1": {"EAPI": "8"},
            "app-misc/user-1": {"EAPI": "8", "RDEPEND": "app-misc/old"},
            "app-misc/sold-1": {"EAPI": "8"},
            "app-misc/suser-1": {"EAPI": "8", "RDEPEND": "app-misc/sold"},
            "app-misc/sholder-1": {"EAPI": "8", "RDEPEND": "!app-misc/sfresh"},
            "app-misc/p-1": {"EAPI": "8"},
            "dev-libs/q-1": {"EAPI": "8", "SLOT": "1"},
            "dev-libs/lib-1": {"EAPI": "8"},
            "app-misc/app-1": {"EAPI": "8", "RDEPEND": "dev-libs/lib"},
            "app-misc/pair-x-1": {"EAPI": "8", "RDEPEND": "!app-misc/pair-y"},
            "app-misc/pair-y-1": {"EAPI": "8"},
            "app-misc/anyuser-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( app-misc/alt-x app-misc/alt-y )",
            },
            "app-misc/alt-x-1": {"EAPI": "8"},
            "app-misc/alt-y-1": {"EAPI": "8"},
            "app-misc/needy-1": {"EAPI": "8"},
            "app-misc/orph-1": {"EAPI": "8", "RDEPEND": "app-misc/lone"},
            "app-misc/lone-1": {"EAPI": "8"},
        },
    },
    # What emerge refuses: a dependency no ebuild satisfies, of an argument, of what it pulls in
    # (another version of which does), two levels down, of an update and of the one version
    # left, of an installed package, as a build-time or post dependency, where only a masked
    # ebuild matches, and behind a || another alternative satisfies; ebuilds with invalid
    # metadata (a conditional on a flag outside IUSE), alone, beside a valid version, and as an
    # update.
    "refused": {
        # Dependencies nothing satisfies hold updates back, which the targets leave to plans.
        "held": True,
        "world": [
            "app-misc/upd",
            "app-misc/lastupd",
            "app-misc/broken",
            "app-misc/brokendep",
            "app-misc/holder",
            "app-misc/invupd",
            "app-misc/deepupd",
        ],
        "ebuilds": {
            "app-misc/argfb-1": {"EAPI": "8"},
            "app-misc/argfb-2": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/puller-1": {"EAPI": "8", "RDEPEND": "dev-libs/pulled"},
            "dev-libs/pulled-1": {"EAPI": "8"},
            "dev-libs/pulled-2": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/chain-1": {"EAPI": "8", "RDEPEND": "dev-libs/link"},
            "dev-libs/link-1": {"EAPI": "8", "RDEPEND": "dev-libs/end"},
            "dev-libs/end-1": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/deepupd-1": {"EAPI": "8"},
            "app-misc/deepupd-2": {"EAPI": "8", "RDEPEND": "dev-libs/link"},
            "app-misc/upd-1": {"EAPI": "8"},
            "app-misc/upd-2": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/lastupd-2": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/broken-1": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/brokendep-1": {"EAPI": "8", "RDEPEND": "dev-libs/gone"},
            "app-misc/wantsmasked-1": {"EAPI": "8", "RDEPEND": "dev-libs/testing"},
            "dev-libs/testing-1": {"EAPI": "8", "KEYWORDS": "~x86"},
            "app-misc/choice-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( dev-libs/missing dev-libs/there )",
            },
            "dev-libs/there-1": {"EAPI": "8"},
            "app-misc/builddep-1": {"EAPI": "8", "BDEPEND": "dev-libs/missing"},
            "app-misc/postdep-1": {"EAPI": "8", "PDEPEND": "dev-libs/missing"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "dev-libs/held"},
            "dev-libs/held-1": {"EAPI": "8"},
            "dev-libs/held-2": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/inv-1": {"EAPI": "8", "RDEPEND": "foo? ( dev-libs/there )"},
            "app-misc/invfb-1": {"EAPI": "8"},
            "app-misc/invfb-2": {"EAPI": "8", "RDEPEND": "foo? ( dev-libs/there )"},
            "app-misc/invupd-1": {"EAPI": "8"},
            "app-misc/invupd-2": {"EAPI": "8", "LICENSE": "foo? ( MIT )"},
        },
        "installed": {
            "app-misc/upd-1": {"EAPI": "8"},
            "app-misc/lastupd-1": {"EAPI": "8"},
            "app-misc/broken-1": {"EAPI": "8", "RDEPEND": "dev-libs/missing"},
            "app-misc/brokendep-1": {"EAPI": "8", "RDEPEND": "dev-libs/gone"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "dev-libs/held"},
            "dev-libs/held-1": {"EAPI": "8"},
            "app-misc/invupd-1": {"EAPI": "8"},
            "app-misc/deepupd-1": {"EAPI": "8"},
        },
    },
    "required": {
        # REQUIRED_USE its USE leaves unsatisfied refuses an update, which the targets leave to
        # plans.
        "held": True,
        "world": ["app-misc/requpd", "app-misc/holder", "dev-libs/held"],
        "ebuilds": {
            "app-misc/req-1": {
                "EAPI": "8",
                "IUSE": "a b",
                "REQUIRED_USE": "^^ ( a b )",
            },
            "app-misc/reqok-1": {
                "EAPI": "8",
                "IUSE": "+a b",
                "REQUIRED_USE": "^^ ( a b )",
            },
            "app-misc/requpd-1": {"EAPI": "8", "IUSE": "a"},
            "app-misc/requpd-2": {"EAPI": "8", "IUSE": "a", "REQUIRED_USE": "a"},
            "app-misc/reqdep-1": {"EAPI": "8", "RDEPEND": "app-misc/req"},
            "app-misc/reqchoice-1": {
                "EAPI": "8",
                "RDEPEND": "|| ( app-misc/req app-misc/reqok )",
            },
            "app-misc/reqcond-1": {
                "EAPI": "8",
                "IUSE": "+x a b c",
                "REQUIRED_USE": "x? ( || ( a b ) ) c? ( a ) !x? ( b )",
            },
            "app-misc/reqold-1": {"EAPI": "6", "IUSE": "a", "REQUIRED_USE": "|| ( )"},
            "dev-libs/held-1": {"EAPI": "8", "IUSE": "a"},
            "dev-libs/held-2": {"EAPI": "8", "IUSE": "a", "REQUIRED_USE": "a"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "<dev-libs/held-2"},
            "app-misc/fb-1": {"EAPI": "8"},
            "app-misc/fb-2": {
                "EAPI": "8",
                "IUSE": "a",
                "REQUIRED_USE": "a",
                "RDEPEND": "dev-libs/missing",
            },
            "dev-libs/badreq-1": {"EAPI": "8", "IUSE": "a", "REQUIRED_USE": "a"},
            "app-misc/rev-1": {"EAPI": "8"},
            "app-misc/rev-2": {
                "EAPI": "8",
                "RDEPEND": "dev-libs/badreq dev-libs/missing",
            },
            "app-misc/kinds-1": {"EAPI": "8"},
            "app-misc/kinds-2": {
                "EAPI": "8",
                "RDEPEND": "dev-libs/missing",
                "DEPEND": "dev-libs/badreq",
            },
            "app-misc/kinds2-1": {"EAPI": "8"},
            "app-misc/kinds2-2": {
                "EAPI": "8",
                "RDEPEND": "dev-libs/badreq",
                "DEPEND": "dev-libs/missing",
            },
            "app-misc/pd-1": {"EAPI": "8"},
            "app-misc/pd-2": {
                "EAPI": "8",
                "RDEPEND": "dev-libs/missing",
                "PDEPEND": "dev-libs/badreq",
            },
        },
        "installed": {
            "app-misc/requpd-1": {"EAPI": "8", "IUSE": "a"},
            "dev-libs/held-1": {"EAPI": "8", "IUSE": "a"},
            "app-misc/holder-1": {"EAPI": "8", "RDEPEND": "<dev-libs/held-2"},
        },
    },
}
