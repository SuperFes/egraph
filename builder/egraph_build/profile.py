"""The profile's implicit IUSE, which decides how USE-dependency defaults apply to installed
packages (a flag counts as in IUSE when the ebuild lists it or the profile implies it).

Portage keeps it behind private config attributes; this is the one place that reads them, so
drift in portage breaks this module and nothing else.
"""

from typing import NamedTuple

from portage.eapi import eapi_has_iuse_effective

# USE_EXPAND_HIDDEN variables become "<name>_.*" patterns; everything else is a literal flag.
_WILDCARD = ".*"


class ImplicitIuse(NamedTuple):
    # IUSE_EFFECTIVE, for packages whose EAPI has it (5 and later).
    effective: tuple = ()
    # Earlier EAPIs: flags implied exactly, and prefixes implying every flag that starts with them.
    literals: tuple = ()
    prefixes: tuple = ()


def implicit_iuse(settings):
    literals = []
    prefixes = []
    for pattern in settings._get_implicit_iuse():
        if pattern.endswith(_WILDCARD):
            prefixes.append(pattern[: -len(_WILDCARD)])
        else:
            literals.append(pattern)
    return ImplicitIuse(
        effective=tuple(sorted(settings._iuse_effective)),
        literals=tuple(sorted(literals)),
        prefixes=tuple(sorted(prefixes)),
    )


def has_iuse_effective(eapi):
    return eapi_has_iuse_effective(eapi)
