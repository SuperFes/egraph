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


def _invalid_messages(metadata, path=lambda key: key):
    """Package.invalid's messages for an installed package: a string portage cannot parse even
    leniently, as it reads an installed package's (no EAPI rules, no IUSE check), a string's
    naming path(key), the file holding it."""
    messages = []
    for key in DEP_KINDS + ("LICENSE", "PROPERTIES", "RESTRICT"):
        try:
            use_reduce(
                metadata[key],
                matchall=True,
                token_class=Atom if key in DEP_KINDS else None,
                flat=True,
            )
        except InvalidDependString as e:
            messages.append(f"{key}: {e} in '{path(key)}'")
    for key in ("PROVIDES", "REQUIRES"):
        try:
            tuple(parse_soname_deps(metadata[key]))
        except InvalidData as e:
            messages.append(f"{key}: {e}")
    return messages


def _invalid(metadata):
    return bool(_invalid_messages(metadata))


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


class _Installed:
    """What portage's _getmaskingstatus reads of emerge's Package for an installed one."""

    installed = True

    def __init__(self, cpv, metadata):
        self.cpv = cpv
        self._metadata = metadata


def reasons(settings, vardb, portdb, cpv, metadata):
    """Why emerge masks an installed package with metadata (KEYS), as its get_masking_status
    words it: portage's own reasons, then each invalid string and an undefined SLOT."""
    from portage.package.ebuild.getmaskingstatus import _getmaskingstatus

    try:
        pkg = _pkg_str(cpv, metadata=metadata, settings=settings)
        found = [
            reason.message
            for reason in _getmaskingstatus(_Installed(pkg, metadata), settings, portdb)
        ]
    except (InvalidData, ValueError):
        found = []
    path = lambda key: vardb.getpath(cpv, filename=key)  # noqa: E731
    found.extend(f"invalid: {message}" for message in _invalid_messages(metadata, path))
    if not metadata["SLOT"]:
        found.append("invalid: SLOT is undefined")
    return tuple(found)


def mask_comment(settings, portdb, cpv, metadata):
    """(file, comment) of the package.mask entry masking an installed package, as emerge shows
    them; empty strings when there is none."""
    import portage

    comment, filename = portage.getmaskingreason(
        cpv, metadata=metadata, settings=settings, portdb=portdb, return_location=True
    )
    return filename or "", comment or ""


def categories(settings, cpv, metadata):
    """Package.masks' keys for an installed package with metadata (KEYS), as depgraph's
    Package._compute_masks finds them: invalid, CHOST, EAPI.unsupported, EAPI.deprecated,
    KEYWORDS, PROPERTIES, RESTRICT, package.mask and LICENSE."""
    try:
        pkg = _pkg_str(cpv, metadata=metadata, settings=settings)
    except InvalidData:
        return frozenset({"invalid"})
    found = set()
    if hasattr(pkg, "slot_invalid") or _invalid(metadata):
        found.add("invalid")
    if not settings._accept_chost(pkg, metadata):
        found.add("CHOST")
    eapi = metadata["EAPI"]
    if not eapi_is_supported(eapi):
        found.add("EAPI.unsupported")
    if _eapi_is_deprecated(eapi):
        found.add("EAPI.deprecated")
    if settings._getMissingKeywords(pkg, metadata):
        found.add("KEYWORDS")
    for key, missing in (
        ("PROPERTIES", settings._getMissingProperties),
        ("RESTRICT", settings._getMissingRestrict),
    ):
        # A string that does not parse made the package invalid above.
        try:
            if missing(pkg, metadata):
                found.add(key)
        except InvalidDependString:
            pass
    if settings._getMaskAtom(pkg, metadata) is not None:
        found.add("package.mask")
    try:
        if settings._getMissingLicenses(pkg, metadata):
            found.add("LICENSE")
    except InvalidDependString:
        pass
    return frozenset(found)


def masked(settings, cpv, metadata):
    """Whether an installed package with metadata (KEYS) has any of Package.masks: invalid
    metadata, an unaccepted CHOST, an unsupported or deprecated EAPI, keywords, properties,
    restrictions, package.mask or a license the configuration does not accept."""
    return bool(categories(settings, cpv, metadata))


# Not visible as installed: Package._eval_visibility passes over the rest for one.
_HIDING = frozenset({"EAPI.unsupported", "invalid", "package.mask", "LICENSE"})


def hidden(found):
    """How emerge sees an installed package masked by found (categories): 0 visible, 1 not, 2
    not and LICENSE among its masks, which emerge warns of whatever its graph holds."""
    if "LICENSE" in found:
        return 2
    return int(bool(found & _HIDING))
