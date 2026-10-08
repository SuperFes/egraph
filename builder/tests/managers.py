"""The visibility configuration as portage's managers hold it, in the shape the shadow binary
gives egraph's stacking of the visibility ledger (src/visibility_stack.cpp): the reference that
stacking is held to. The keys of a {cp: {atom: tokens}} dict come plain cps first, as a lookup
collects them."""


def net(tokens):
    """Incremental tokens (x, -x, *, -*) as their net effect: the last * or -* if any, then the
    last word on each name after it, sorted. A license group's expansion comes from a set, so
    its order would change from one run to the next."""
    reset = None
    last = {}
    for token in tokens:
        if token in ("*", "-*"):
            reset = token
            last.clear()
        else:
            last[token.lstrip("-")] = token
    return ([reset] if reset else []) + sorted(last.values())


def _layer(cpdict):
    """The keys of a plain {cp: {atom: tokens}} dict, in its order."""
    return [
        {"atom": str(atom), "tokens": list(tokens)}
        for atoms in cpdict.values()
        for atom, tokens in atoms.items()
    ]


def _extended(atom_dict, tokens_of=list):
    """The keys of an ExtendedAtomDict of {atom: tokens}."""
    return [
        {"atom": str(atom), "tokens": tokens_of(tokens)}
        for _, atoms in atom_dict.iteritems()
        for atom, tokens in atoms.items()
    ]


def _atoms(atom_dict):
    """The atoms of an ExtendedAtomDict of lists."""
    return [str(atom) for _, atoms in atom_dict.iteritems() for atom in atoms]


def read(settings):
    keywords = settings._keywords_manager
    masks = settings._mask_manager
    licenses = settings._license_manager
    return {
        "accept_keywords": settings.get("ACCEPT_KEYWORDS", "").split(),
        "environment_keywords": settings.configdict["backupenv"]
        .get("ACCEPT_KEYWORDS", "")
        .split(),
        "profile_keywords": [_layer(layer) for layer in keywords._pkeywords_list],
        "profile_accept_keywords": [
            _layer(layer) for layer in keywords._p_accept_keywords
        ],
        "accept_keywords_entries": _extended(keywords.pkeywordsdict),
        "masks": _atoms(masks._pmaskdict),
        "unmasks": _atoms(masks._punmaskdict),
        "accept_license": net(licenses._accept_license),
        "licenses": _extended(licenses._plicensedict, net),
        "accept_properties": list(settings._accept_properties),
        "properties": _extended(settings._ppropertiesdict),
        "accept_restrict": list(settings._accept_restrict),
        "restrict": _extended(settings._paccept_restrict),
    }
