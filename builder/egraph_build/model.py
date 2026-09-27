"""Vocabulary shared by the installed layer and the portage oracle."""

from typing import NamedTuple

DEP_KINDS = ("BDEPEND", "DEPEND", "IDEPEND", "PDEPEND", "RDEPEND")


class Edge(NamedTuple):
    """An installed package depending on another through one atom of one kind."""

    parent: str
    child: str
    kind: str
    atom: str
    # The atom is an alternative inside a || group, so the parent may be using another one.
    choice: bool


class SonameUse(NamedTuple):
    cpv: str
    multilib_category: str
