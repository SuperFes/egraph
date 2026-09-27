"""The installed layer: installed packages, their USE-reduced dependency trees,
and the installed packages each atom resolves to.

It answers the same queries as egraph_build.oracle, with the vardb argument
dropped, from an index built in one pass.
"""

from egraph_build.model import DEP_KINDS


class InstalledLayer:
    def installed(self):
        raise NotImplementedError

    def matches(self, atom):
        raise NotImplementedError

    def errors(self):
        raise NotImplementedError

    def deps(self, cpv, kinds=DEP_KINDS):
        raise NotImplementedError

    def rdeps(self, cpv, kinds=DEP_KINDS):
        raise NotImplementedError

    def soname_providers(self, soname):
        raise NotImplementedError

    def soname_consumers(self, soname):
        raise NotImplementedError


def build(vardb):
    """Evaluate every installed package in vardb into an InstalledLayer."""
    raise NotImplementedError("installed layer: roadmap step 2")
