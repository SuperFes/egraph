"""Whether emerge masks an installed package: depgraph's Package._compute_masks, over the metadata
emerge reads for the package, through the config's own checks.

Those checks are private to portage's config; this module is the one place egraph calls them.
"""

from portage import _eapi_is_deprecated, eapi_is_supported
from portage.dep import Atom, use_reduce
from portage.dep.soname.parse import parse_soname_deps
from portage.exception import InvalidData, InvalidDependString
from portage.versions import _pkg_str

from egraph_build.model import DEP_KINDS

# What the checks read of a package.
KEYS = DEP_KINDS + (
    "CHOST",
    "EAPI",
    "KEYWORDS",
    "LICENSE",
    "PROPERTIES",
    "PROVIDES",
    "REQUIRES",
    "RESTRICT",
    "SLOT",
    "USE",
    "repository",
)


def _invalid(metadata):
    """Package.invalid for an installed package: a string portage cannot parse even leniently,
    as it reads an installed package's (no EAPI rules, no IUSE check)."""
    for key in DEP_KINDS + ("LICENSE", "PROPERTIES", "RESTRICT"):
        try:
            use_reduce(
                metadata[key],
                matchall=True,
                token_class=Atom if key in DEP_KINDS else None,
                flat=True,
            )
        except InvalidDependString:
            return True
    for key in ("PROVIDES", "REQUIRES"):
        try:
            tuple(parse_soname_deps(metadata[key]))
        except InvalidData:
            return True
    return False


def masked(settings, cpv, metadata):
    """Whether an installed package with metadata (KEYS) has any of Package.masks: invalid
    metadata, an unaccepted CHOST, an unsupported or deprecated EAPI, keywords, properties,
    restrictions, package.mask or a license the configuration does not accept."""
    try:
        pkg = _pkg_str(cpv, metadata=metadata, settings=settings)
    except InvalidData:
        return True
    if hasattr(pkg, "slot_invalid") or _invalid(metadata):
        return True
    eapi = metadata["EAPI"]
    if not eapi_is_supported(eapi) or _eapi_is_deprecated(eapi):
        return True
    if not settings._accept_chost(pkg, metadata):
        return True
    if settings._getMissingKeywords(pkg, metadata):
        return True
    if settings._getMaskAtom(pkg, metadata) is not None:
        return True
    for missing in (
        settings._getMissingProperties,
        settings._getMissingRestrict,
        settings._getMissingLicenses,
    ):
        # A string that does not parse made the package invalid above.
        if missing(pkg, metadata):
            return True
    return False
