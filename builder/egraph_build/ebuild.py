"""config.setcpv for an ebuild as emerge hands it its Package.

Given a cpv string and a metadata dict, setcpv stacks the repository's USE layer but matches the
repository's use.force, use.mask and their package and stable files against a cpv without a
repository, so they never apply; emerge's Package carries its repository and gets them. Portage
keeps the Package path to _emerge; this is the one place that stands in for it.
"""


class _Iuse:
    def __init__(self, iuse):
        self.all = frozenset(flag.lstrip("+-") for flag in iuse.split())


class _Ebuild:
    """What setcpv reads of a Package for an ebuild; the rest is its cpv's (cp, repo, slot,
    version, stable)."""

    built = False
    installed = False

    def __init__(self, cpv, metadata, settings):
        from portage.versions import _pkg_str

        self.cpv = _pkg_str(cpv, metadata=metadata, settings=settings)
        self._metadata = metadata
        self._raw_metadata = metadata
        self.iuse = _Iuse(metadata["IUSE"])

    def __getattr__(self, name):
        return getattr(self.cpv, name)


def set_ebuild(settings, cpv, metadata):
    """settings set up for the ebuild of cpv, as emerge sets them for its Package; metadata
    holds what setcpv reads (evaluated._CANDIDATE_KEYS), its repository among it.

    Returns the stand-in, for the caller to keep until its next call: setcpv skips a call whose
    cpv and package id() match its last one's, and a freed stand-in would lend its id to the
    next.
    """
    pkg = _Ebuild(cpv, metadata, settings)
    settings.setcpv(pkg)
    return pkg
