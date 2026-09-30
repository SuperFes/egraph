"""Whether emerge masks an installed package: depgraph's Package._compute_masks, over the metadata
emerge reads for the package, through the config's own checks; and which ebuilds depgraph finds
invalid.

Those checks are private to portage's config; this module is the one place egraph calls them.
"""

from portage import _eapi_is_deprecated, eapi_is_supported
from portage.dep import Atom, check_required_use, use_reduce
from portage.dep.soname.parse import parse_soname_deps
from portage.eapi import _get_eapi_attrs
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


# What invalid_ebuild reads of an ebuild.
EBUILD_KEYS = DEP_KINDS + (
    "EAPI",
    "IUSE",
    "LICENSE",
    "PROPERTIES",
    "REQUIRED_USE",
    "RESTRICT",
    "SLOT",
    "SRC_URI",
    "repository",
)


def invalid_ebuild(portdb, cpv, metadata):
    """Package.invalid's messages for an ebuild with metadata (EBUILD_KEYS), as depgraph words
    them after "invalid: ": strings that do not parse under its EAPI, conditionals on flags
    outside its IUSE, and the like. portdb's match-visible passes over them, depgraph does not.
    """
    messages = []
    try:
        pkg = _pkg_str(cpv, metadata=metadata, settings=portdb.settings)
    except InvalidData:
        return [f"invalid CPV: {cpv}"]
    if hasattr(pkg, "slot_invalid"):
        messages.append(f"SLOT: invalid value: '{metadata['SLOT']}'")
    eapi = metadata["EAPI"]
    attrs = _get_eapi_attrs(eapi)
    tokens = metadata["IUSE"].split()
    iuse = frozenset(token.lstrip("+-") for token in tokens)
    if not attrs.iuse_defaults and any(token[:1] in "+-" for token in tokens):
        messages.append("IUSE contains defaults, but EAPI doesn't allow them")
    implicit = portdb._iuse_implicit_cnstr(pkg, metadata)

    def valid(flag):
        return flag in iuse or implicit(flag)

    def check(key, **options):
        try:
            return use_reduce(
                metadata[key],
                eapi=eapi,
                matchall=True,
                is_valid_flag=valid,
                flat=True,
                **options,
            )
        except InvalidDependString as e:
            categorized = [
                f"{key}: {error}"
                for error in e.errors or ()
                if getattr(error, "category", None) is not None
            ]
            messages.extend(categorized or [f"{key}: {e}"])
            return ()

    for key in DEP_KINDS:
        if metadata[key]:
            for atom in check(key, token_class=Atom):
                if isinstance(atom, Atom) and atom.slot_operator_built:
                    messages.append(
                        f'{key}: Improper context for slot-operator "built" atom syntax:'
                        f" {atom.unevaluated_atom}"
                    )
    for key in ("LICENSE", "PROPERTIES", "RESTRICT"):
        if metadata[key]:
            check(key)
    if metadata["REQUIRED_USE"]:
        if not attrs.required_use:
            messages.append(f"REQUIRED_USE set, but EAPI='{eapi}' doesn't allow it")
        else:
            try:
                check_required_use(metadata["REQUIRED_USE"], (), valid, eapi=eapi)
            except InvalidDependString as e:
                messages.append(f"REQUIRED_USE: {e}")
    if metadata["SRC_URI"]:
        check("SRC_URI", is_src_uri=True)
    return messages


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
