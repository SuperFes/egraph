"""What each package on a running emerge's merge list waits for.

emerge records the packages it has left to merge in mtimedb's resume mergelist. A package waits
for those of the others that its build- and install-time dependencies, and its run-time ones,
match: each has to be merged first. PDEPEND does not hold a merge back. Dependencies are reduced
under the USE the package will be built with (an ebuild) or was built with (a binary package).

Within an any-of group every pending member counts: the resolver picked one, and which is not
recorded, so this may name a wait the scheduler does not have.
"""

import json
from typing import Dict, Iterable, List, NamedTuple

# PDEPEND is merged after the package it belongs to, so it never holds one back.
WAIT_KINDS = ("DEPEND", "BDEPEND", "RDEPEND", "IDEPEND")


class Entry(NamedTuple):
    # "ebuild" or "binary", as the mergelist names the package's type.
    kind: str
    cpv: str


def parse_entry(text):
    """An entry from "kind:cpv" on the command line."""
    kind, _, cpv = text.partition(":")
    if kind not in ("ebuild", "binary") or not cpv:
        raise ValueError(f"not an ebuild: or binary: entry: {text}")
    return Entry(kind, cpv)


def _atoms(tree):
    """Every atom in a reduced dependency tree, any-of groups included; blockers are not waits."""
    for token in tree:
        if isinstance(token, list):
            yield from _atoms(token)
        elif token != "||" and not token.blocker:
            yield token


def _metadata(entry, portdb, bindb):
    """The entry's metadata, or None when its ebuild or binary package is gone."""
    from portage.exception import PortageException

    keys = list(WAIT_KINDS) + ["EAPI", "SLOT", "USE"]
    db = bindb if entry.kind == "binary" else portdb
    try:
        return dict(zip(keys, db.aux_get(entry.cpv, keys)))
    except (KeyError, PortageException):
        return None


def _use(entry, metadata, settings, portdb):
    if entry.kind == "binary":
        return frozenset(metadata["USE"].split())
    from egraph_build import ebuild, evaluated

    keys = list(evaluated._CANDIDATE_KEYS)
    # The ebuild portdb picks, as setcpv given portdb would: the same for the same cpv.
    ebuild.set_ebuild(
        settings, entry.cpv, dict(zip(keys, portdb.aux_get(entry.cpv, keys)))
    )
    return frozenset(settings["PORTAGE_USE"].split())


def waits(settings, portdb, bindb, entries: Iterable[Entry]) -> Dict[str, List[str]]:
    """For every entry's cpv, the other entries' cpvs it waits for, sorted."""
    from portage import config
    from portage.dep import Atom, match_from_list, use_reduce

    entries = list(entries)
    settings = config(clone=settings)
    metadata = {entry.cpv: _metadata(entry, portdb, bindb) for entry in entries}
    # A package whose ebuild went away (a sync during the emerge) waits for nothing it can name.
    result = {cpv: [] for cpv, meta in metadata.items() if meta is None}
    metadata = {cpv: meta for cpv, meta in metadata.items() if meta is not None}
    # match_from_list takes "cpv:slot" strings, which is enough to match slots and sub-slots.
    slotted = {f"{cpv}:{meta['SLOT']}": cpv for cpv, meta in metadata.items()}
    for entry in entries:
        meta = metadata.get(entry.cpv)
        if meta is None:
            continue
        use = _use(entry, meta, settings, portdb)
        found = set()
        for kind in WAIT_KINDS:
            tree = use_reduce(
                meta[kind], uselist=use, eapi=meta["EAPI"], token_class=Atom
            )
            for atom in _atoms(tree):
                for match in match_from_list(atom, list(slotted)):
                    found.add(slotted[match])
        found.discard(entry.cpv)
        result[entry.cpv] = sorted(found)
    return result


def to_json(result):
    return json.dumps(result, indent=1, sort_keys=True) + "\n"
