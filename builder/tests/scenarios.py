"""Throwaway installed systems, as ResolverPlayground keyword arguments.

Every comparative test runs against every scenario, so a case added here is
checked against portage by every query at once.
"""

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
    # Sonames across multilib categories.
    "sonames": {
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
