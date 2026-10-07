"""The store file: encoding, decoding and atomic replacement (docs/store-format.md)."""

import os
import struct
import tempfile
from typing import NamedTuple

from egraph_build.evaluated import Candidate, Dependencies, EvaluatedLayer, Possible
from egraph_build.installed import InstalledLayer, Node, Package
from egraph_build.model import DEP_KINDS
from egraph_build.profile import ImplicitIuse, has_iuse_effective
from egraph_build.repository import (
    Advisory,
    AdvisoryPackage,
    Eapi,
    Entry,
    Repository,
    RepositoryIndex,
    Version,
    Visibility,
)
from egraph_build.roots import Root

MAGIC = b"EGRAPH\0\0"
FORMAT_VERSION = 6
(
    SECTION_META,
    SECTION_INPUTS,
    SECTION_STRINGS,
    SECTION_PACKAGES,
    SECTION_ROOTS,
    SECTION_PROFILE,
) = range(1, 7)
SECTIONS = (
    SECTION_META,
    SECTION_INPUTS,
    SECTION_STRINGS,
    SECTION_PACKAGES,
    SECTION_ROOTS,
    SECTION_PROFILE,
)
INPUT_FILE, INPUT_DIRECTORY, INPUT_SYMLINK, INPUT_MISSING = range(4)
DEFAULT_PATH = "var/cache/egraph/installed.egraph"

EVALUATED_MAGIC = b"EGRAPHEV"
EVALUATED_FORMAT_VERSION = 10
(
    SECTION_DEPENDENCIES,
    SECTION_CANDIDATES,
    SECTION_REPOSITORY,
    SECTION_REQUESTED,
    SECTION_USE_EXPAND,
) = range(4, 9)
EVALUATED_SECTIONS = (
    SECTION_META,
    SECTION_INPUTS,
    SECTION_STRINGS,
    SECTION_DEPENDENCIES,
    SECTION_CANDIDATES,
    SECTION_REPOSITORY,
    SECTION_REQUESTED,
    SECTION_USE_EXPAND,
)

REPOSITORY_MAGIC = b"EGRAPHRI"
REPOSITORY_FORMAT_VERSION = 4
SECTION_REPOSITORIES, SECTION_VERSIONS, SECTION_VISIBILITY, SECTION_ADVISORIES = range(
    4, 8
)
REPOSITORY_SECTIONS = (
    SECTION_META,
    SECTION_INPUTS,
    SECTION_STRINGS,
    SECTION_REPOSITORIES,
    SECTION_VERSIONS,
    SECTION_VISIBILITY,
    SECTION_ADVISORIES,
)

_HEADER = struct.Struct("<8sII")
_ENTRY = struct.Struct("<IQQ")


class StoreError(ValueError):
    pass


class Meta(NamedTuple):
    egraph_version: str
    portage_version: str
    eroot: str
    build_time_ns: int
    implicit: ImplicitIuse = ImplicitIuse()


class EvaluatedMeta(NamedTuple):
    egraph_version: str
    portage_version: str
    eroot: str
    build_time_ns: int
    # The build start of the installed store whose package ids this one uses.
    installed_build_time_ns: int


class RepositoryMeta(NamedTuple):
    egraph_version: str
    portage_version: str
    eroot: str
    build_time_ns: int


class Input(NamedTuple):
    path: str
    kind: int
    mtime_ns: int
    size: int


def default_path(eroot):
    return os.path.join(eroot, DEFAULT_PATH)


def evaluated_path(path):
    """The evaluated store beside an installed store: its last extension becomes .evaluated.egraph."""
    root, _ = os.path.splitext(os.fspath(path))
    return root + ".evaluated.egraph"


def repository_path(path):
    """The repository index beside an installed store: its last extension becomes
    .repository.egraph."""
    root, _ = os.path.splitext(os.fspath(path))
    return root + ".repository.egraph"


def _bytes(text):
    # surrogateescape round-trips whatever bytes portage decoded into str.
    return text.encode("utf-8", "surrogateescape")


class _Writer:
    def __init__(self):
        self.out = bytearray()

    def varint(self, value):
        out = self.out
        if value < 0x80:
            out.append(value)
            return
        while value > 0x7F:
            out.append((value & 0x7F) | 0x80)
            value >>= 7
        out.append(value)

    def raw(self, data):
        self.varint(len(data))
        self.out += data

    def text(self, value):
        self.raw(_bytes(value))

    def ids(self, values):
        self.varint(len(values))
        for value in values:
            self.varint(value)


class _Strings:
    def __init__(self):
        self.table = {"": 0}

    def __call__(self, value):
        index = self.table.get(value)
        if index is None:
            index = self.table[value] = len(self.table)
        return index


def _write_trees(w, deps, strings, index):
    for nodes in deps:
        w.varint(len(nodes))
        for node in nodes:
            w.varint(node.type)
            w.varint(node.parent + 1)
            w.varint(strings(node.atom))
            w.ids([index[cpv] for cpv in node.matches])


def _write_inputs(inputs):
    w = _Writer()
    w.varint(len(inputs))
    for item in inputs:
        w.text(item.path)
        w.varint(item.kind)
        w.varint(item.mtime_ns)
        w.varint(item.size)
    return w.out


def _write_strings(strings):
    w = _Writer()
    w.varint(len(strings.table))
    for value in strings.table:
        w.text(value)
    return w.out


def _frame(magic, version, order, sections):
    offset = _HEADER.size + _ENTRY.size * len(sections)
    table = bytearray()
    body = bytearray()
    for section_id in order:
        data = sections[section_id]
        table += _ENTRY.pack(section_id, offset + len(body), len(data))
        body += data
    return _HEADER.pack(magic, version, len(sections)) + table + body


def encode(layer, meta, inputs=()):
    """The store bytes for an InstalledLayer, its Meta and its Inputs."""
    strings = _Strings()
    packages = list(layer)
    index = {pkg.cpv: i for i, pkg in enumerate(packages)}
    providers = {}
    for i, pkg in enumerate(packages):
        for soname in pkg.provides:
            providers.setdefault(soname, []).append(i)

    sections = {}

    w = _Writer()
    for value in (meta.egraph_version, meta.portage_version, meta.eroot):
        w.text(value)
    w.varint(meta.build_time_ns)
    sections[SECTION_META] = w.out

    sections[SECTION_INPUTS] = _write_inputs(inputs)

    w = _Writer()
    w.varint(len(packages))
    for pkg in packages:
        for value in (pkg.cpv, pkg.cp, pkg.slot, pkg.sub_slot, pkg.repo, pkg.eapi):
            w.varint(strings(value))
        w.varint(1 if has_iuse_effective(pkg.eapi) else 0)
        w.varint(pkg.counter)
        w.varint(pkg.merged)
        w.ids([strings(flag) for flag in pkg.use])
        w.ids([strings(flag) for flag in pkg.iuse])
        w.varint(len(pkg.errors))
        for key, message in pkg.errors:
            w.varint(strings(key))
            w.varint(strings(message))
        _write_trees(w, pkg.deps, strings, index)
        w.varint(len(pkg.provides))
        for category, soname in pkg.provides:
            w.varint(strings(category))
            w.varint(strings(soname))
        w.varint(len(pkg.requires))
        for category, soname in pkg.requires:
            w.varint(strings(category))
            w.varint(strings(soname))
            w.ids(providers.get((category, soname), ()))
    sections[SECTION_PACKAGES] = w.out

    roots = layer.roots()
    w = _Writer()
    w.varint(len(roots))
    for root in roots:
        w.varint(strings(root.set))
        w.varint(strings(root.atom))
        w.varint(strings(root.via))
        w.ids([index[cpv] for cpv in root.matches])
    sections[SECTION_ROOTS] = w.out

    sections[SECTION_STRINGS] = _write_strings(strings)

    w = _Writer()
    for flags in meta.implicit:
        w.varint(len(flags))
        for flag in flags:
            w.text(flag)
    sections[SECTION_PROFILE] = w.out

    return _frame(MAGIC, FORMAT_VERSION, SECTIONS, sections)


def encode_evaluated(layer, meta, inputs=()):
    """The evaluated store bytes for an EvaluatedLayer, its EvaluatedMeta and its Inputs."""
    strings = _Strings()
    index = {cpv: i for i, cpv in enumerate(layer.installed())}
    candidates = layer.candidates()
    candidate_index = {(c.cpv, c.repo): i for i, c in enumerate(candidates)}
    sections = {}

    w = _Writer()
    for value in (meta.egraph_version, meta.portage_version, meta.eroot):
        w.text(value)
    w.varint(meta.build_time_ns)
    w.varint(meta.installed_build_time_ns)
    sections[SECTION_META] = w.out

    sections[SECTION_INPUTS] = _write_inputs(inputs)

    w = _Writer()
    w.varint(len(index))
    for pkg in layer:
        w.varint(strings(pkg.cpv))
        w.varint(pkg.source)
        w.varint(strings(pkg.eapi))
        w.varint(len(pkg.errors))
        for key, message in pkg.errors:
            w.varint(strings(key))
            w.varint(strings(message))
        _write_trees(w, pkg.deps, strings, index)
        w.varint(len(pkg.possible))
        for p in pkg.possible:
            w.varint(DEP_KINDS.index(p.kind))
            w.varint(strings(p.atom))
            w.varint(int(p.choice))
            w.ids([index[cpv] for cpv in p.matches])
            w.ids([strings(flag) for flag in p.flags])
        w.varint(int(pkg.visible))
        w.varint(int(pkg.masked))
        w.varint(int(pkg.vdb_masked))
        w.varint(0 if pkg.target is None else candidate_index[pkg.target] + 1)
        w.ids([strings(flag) for flag in pkg.rebuild])
        for reasons in (pkg.mask_reasons, pkg.vdb_mask_reasons):
            w.ids([strings(reason) for reason in reasons])
        w.varint(strings(pkg.mask_file))
        w.varint(strings(pkg.mask_comment))
        w.varint(pkg.hidden)
        w.varint(pkg.vdb_hidden)
    sections[SECTION_DEPENDENCIES] = w.out

    w = _Writer()
    w.varint(len(candidates))
    for c in candidates:
        for value in (c.cp, c.cpv, c.repo, c.slot, c.sub_slot):
            w.varint(strings(value))
        for values in (c.use, c.iuse, c.forced, c.reasons):
            w.ids([strings(value) for value in values])
        w.varint(len(c.errors))
        for key, message in c.errors:
            w.varint(strings(key))
            w.varint(strings(message))
        _write_trees(w, c.deps, strings, index)
        w.ids([strings(token) for token in c.required_use])
        w.varint(int(c.empty_groups_true))
        for kind in c.tokens:
            w.ids([strings(token) for token in kind])
    sections[SECTION_CANDIDATES] = w.out

    for section, cps in (
        (SECTION_REPOSITORY, layer.repository_cps()),
        (SECTION_REQUESTED, layer.requested()),
    ):
        w = _Writer()
        w.ids([strings(cp) for cp in cps])
        sections[section] = w.out

    w = _Writer()
    for names in (layer.use_expand(), layer.use_expand_hidden()):
        w.ids([strings(name) for name in names])
    sections[SECTION_USE_EXPAND] = w.out

    sections[SECTION_STRINGS] = _write_strings(strings)
    return _frame(
        EVALUATED_MAGIC, EVALUATED_FORMAT_VERSION, EVALUATED_SECTIONS, sections
    )


def _write_entries(w, entries, strings):
    w.varint(len(entries))
    for entry in entries:
        w.varint(strings(entry.atom))
        w.ids([strings(token) for token in entry.tokens])


def encode_repository(index, meta, inputs=()):
    """The repository index bytes for a RepositoryIndex, its RepositoryMeta and its Inputs."""
    strings = _Strings()
    repo_index = {r.name: i for i, r in enumerate(index.repositories)}
    sections = {}

    w = _Writer()
    for value in (meta.egraph_version, meta.portage_version, meta.eroot):
        w.text(value)
    w.varint(meta.build_time_ns)
    sections[SECTION_META] = w.out

    sections[SECTION_INPUTS] = _write_inputs(inputs)

    w = _Writer()
    w.varint(len(index.repositories))
    for r in index.repositories:
        w.varint(strings(r.name))
        w.varint(strings(r.location))
        w.varint(int(r.description_index))
    sections[SECTION_REPOSITORIES] = w.out

    w = _Writer()
    w.varint(len(index.versions))
    for v in index.versions:
        for value in (v.cp, v.cpv, v.slot, v.sub_slot, v.eapi):
            w.varint(strings(value))
        w.varint(repo_index[v.repo])
        for values in (v.keywords, v.license, v.properties, v.restrict, v.use):
            w.ids([strings(value) for value in values])
        w.varint(strings(v.description))
        w.varint(strings(v.homepage))
        w.ids([strings(message) for message in v.invalid])
    sections[SECTION_VERSIONS] = w.out

    vis = index.visibility
    w = _Writer()
    w.varint(len(vis.eapis))
    for eapi in vis.eapis:
        w.varint(strings(eapi.eapi))
        w.varint(int(eapi.supported))
        w.varint(int(eapi.deprecated))
    for values in (vis.accept_keywords, vis.environment_keywords):
        w.ids([strings(value) for value in values])
    w.varint(strings(vis.arch))
    for layers in (vis.profile_keywords, vis.profile_accept_keywords):
        w.varint(len(layers))
        for layer in layers:
            _write_entries(w, layer, strings)
    _write_entries(w, vis.accept_keywords_entries, strings)
    for atoms in (vis.masks, vis.unmasks):
        w.ids([strings(atom) for atom in atoms])
    for accepted, entries in (
        (vis.accept_license, vis.licenses),
        (vis.accept_properties, vis.properties),
        (vis.accept_restrict, vis.restrict),
    ):
        w.ids([strings(value) for value in accepted])
        _write_entries(w, entries, strings)
    sections[SECTION_VISIBILITY] = w.out

    w = _Writer()
    w.varint(len(index.advisories))
    for a in index.advisories:
        for value in (a.id, a.title, a.synopsis):
            w.varint(strings(value))
        w.varint(a.revision)
        w.varint(len(a.packages))
        for p in a.packages:
            w.varint(strings(p.cp))
            w.varint(strings(p.arch))
            for atoms in (p.vulnerable, p.unaffected):
                w.ids([strings(atom) for atom in atoms])
    sections[SECTION_ADVISORIES] = w.out

    sections[SECTION_STRINGS] = _write_strings(strings)
    return _frame(
        REPOSITORY_MAGIC, REPOSITORY_FORMAT_VERSION, REPOSITORY_SECTIONS, sections
    )


class _Reader:
    def __init__(self, data, name):
        self.data = data
        self.pos = 0
        self.name = name

    def fail(self, what):
        raise StoreError(f"{self.name}: {what} at byte {self.pos}")

    def varint(self, limit=None):
        pos = self.pos
        if pos < len(self.data) and self.data[pos] < 0x80:
            value = self.data[pos]
            self.pos = pos + 1
            if limit is not None and value >= limit:
                self.fail(f"index {value} out of range {limit}")
            return value
        value = 0
        for shift in range(0, 70, 7):
            if self.pos >= len(self.data):
                self.fail("truncated varint")
            byte = self.data[self.pos]
            self.pos += 1
            value |= (byte & 0x7F) << shift
            if byte < 0x80:
                if limit is not None and value >= limit:
                    self.fail(f"index {value} out of range {limit}")
                return value
        self.fail("varint longer than 10 bytes")

    def count(self):
        value = self.varint()
        if value > len(self.data) - self.pos:
            self.fail(f"count {value} exceeds the bytes left")
        return value

    def raw(self):
        size = self.count()
        start = self.pos
        self.pos += size
        return bytes(self.data[start : self.pos])

    def text(self):
        return self.raw().decode("utf-8", "surrogateescape")

    def ids(self, limit):
        return [self.varint(limit) for _ in range(self.count())]

    def done(self):
        if self.pos != len(self.data):
            self.fail("trailing bytes")


def _sections(data, magic=MAGIC, version=FORMAT_VERSION, ids=SECTIONS):
    if len(data) < _HEADER.size:
        raise StoreError("truncated header")
    found_magic, found_version, count = _HEADER.unpack_from(data)
    if found_magic != magic:
        kinds = {
            MAGIC: "an egraph store",
            EVALUATED_MAGIC: "an evaluated egraph store",
            REPOSITORY_MAGIC: "an egraph repository index",
        }
        raise StoreError(f"not {kinds[magic]}")
    if found_version != version:
        raise StoreError(f"format version {found_version}, expected {version}")
    if count != len(ids) or len(data) < _HEADER.size + _ENTRY.size * count:
        raise StoreError("bad section table")
    found = {}
    for i in range(count):
        section_id, offset, length = _ENTRY.unpack_from(
            data, _HEADER.size + _ENTRY.size * i
        )
        if section_id not in ids or section_id in found:
            raise StoreError(f"unexpected section {section_id}")
        if offset > len(data) or length > len(data) - offset:
            raise StoreError(f"section {section_id} outside the file")
        found[section_id] = memoryview(data)[offset : offset + length]
    return found


def _read_inputs(section):
    r = _Reader(section, "inputs")
    inputs = tuple(
        Input(r.text(), r.varint(), r.varint(), r.varint()) for _ in range(r.count())
    )
    r.done()
    return inputs


def _read_strings(section):
    r = _Reader(section, "strings")
    strings = [r.text() for _ in range(r.count())]
    r.done()
    if not strings or strings[0]:
        r.fail("string 0 must be empty")
    return strings


def _read_trees(r, s, packages, names=None):
    """Node lists per kind, matches named by names when given, else still package ids."""
    deps = []
    for _ in DEP_KINDS:
        nodes = []
        for index in range(r.count()):
            node_type = r.varint(5)
            parent = r.varint(index + 1) - 1
            if parent >= 0 and nodes[parent].type not in (1, 2):
                r.fail("parent is not a group")
            atom = s()
            matches = r.ids(packages)
            if names is not None:
                matches = tuple(names[i] for i in matches)
            nodes.append(Node(node_type, parent, atom, matches))
        deps.append(tuple(nodes))
    return tuple(deps)


def _named(deps, cpvs):
    # Node(...) rather than _replace: this runs for every node of a store.
    return tuple(
        tuple(
            Node(n.type, n.parent, n.atom, tuple(cpvs[i] for i in n.matches))
            for n in nodes
        )
        for nodes in deps
    )


def decode(data):
    """(Meta, Inputs, InstalledLayer) from store bytes; raises StoreError."""
    sections = _sections(data)

    r = _Reader(sections[SECTION_PROFILE], "profile")
    implicit = ImplicitIuse(
        *(tuple(r.text() for _ in range(r.count())) for _ in ImplicitIuse._fields)
    )
    r.done()

    r = _Reader(sections[SECTION_META], "meta")
    meta = Meta(r.text(), r.text(), r.text(), r.varint(), implicit)
    r.done()

    inputs = _read_inputs(sections[SECTION_INPUTS])
    strings = _read_strings(sections[SECTION_STRINGS])

    r = _Reader(sections[SECTION_PACKAGES], "packages")
    count = r.count()
    nstrings = len(strings)

    def s():
        return strings[r.varint(nstrings)]

    raw = []
    for _ in range(count):
        fields = [s() for _ in range(6)]
        # Derived from the EAPI; only the C++ matcher needs it spelled out.
        r.varint(2)
        merge = (r.varint(), r.varint())
        use = tuple(strings[i] for i in r.ids(nstrings))
        iuse = tuple(strings[i] for i in r.ids(nstrings))
        errors = tuple((s(), s()) for _ in range(r.count()))
        deps = _read_trees(r, s, count)
        provides = tuple((s(), s()) for _ in range(r.count()))
        requires = []
        for _ in range(r.count()):
            requires.append((s(), s()))
            r.ids(count)
        raw.append((fields, merge, use, iuse, errors, deps, provides, tuple(requires)))
    r.done()

    r = _Reader(sections[SECTION_ROOTS], "roots")
    raw_roots = []
    for _ in range(r.count()):
        raw_roots.append((s(), s(), s(), r.ids(count)))
    r.done()

    cpvs = [fields[0] for fields, *_ in raw]
    packages = []
    for fields, (counter, merged), use, iuse, errors, deps, provides, requires in raw:
        packages.append(
            Package(
                *fields,
                use=use,
                iuse=iuse,
                errors=errors,
                deps=_named(deps, cpvs),
                provides=provides,
                requires=requires,
                counter=counter,
                merged=merged,
            )
        )
    roots = tuple(
        Root(name, atom, tuple(cpvs[i] for i in matches), via)
        for name, atom, via, matches in raw_roots
    )
    return meta, inputs, InstalledLayer(packages, roots)


def decode_evaluated(data):
    """(EvaluatedMeta, Inputs, EvaluatedLayer) from evaluated store bytes; raises StoreError."""
    sections = _sections(
        data, EVALUATED_MAGIC, EVALUATED_FORMAT_VERSION, EVALUATED_SECTIONS
    )

    r = _Reader(sections[SECTION_META], "meta")
    meta = EvaluatedMeta(r.text(), r.text(), r.text(), r.varint(), r.varint())
    r.done()

    inputs = _read_inputs(sections[SECTION_INPUTS])
    strings = _read_strings(sections[SECTION_STRINGS])
    nstrings = len(strings)

    r = _Reader(sections[SECTION_DEPENDENCIES], "dependencies")

    def s():
        return strings[r.varint(nstrings)]

    count = r.count()
    raw = []
    for _ in range(count):
        cpv = s()
        source = r.varint(3)
        eapi = s()
        errors = tuple((s(), s()) for _ in range(r.count()))
        deps = _read_trees(r, s, count)
        possible = []
        for _ in range(r.count()):
            kind = DEP_KINDS[r.varint(len(DEP_KINDS))]
            atom = s()
            choice = bool(r.varint(2))
            matches = r.ids(count)
            flags = tuple(strings[i] for i in r.ids(nstrings))
            possible.append((kind, atom, choice, matches, flags))
        # visible, masked, vdb_masked, target, rebuild
        weighed = (
            bool(r.varint(2)),
            bool(r.varint(2)),
            bool(r.varint(2)),
            r.varint(),
            tuple(strings[i] for i in r.ids(nstrings)),
            tuple(strings[i] for i in r.ids(nstrings)),
            tuple(strings[i] for i in r.ids(nstrings)),
            s(),
            s(),
            r.varint(3),
            r.varint(3),
        )
        raw.append((cpv, source, eapi, errors, deps, possible, weighed))
    r.done()

    r = _Reader(sections[SECTION_CANDIDATES], "candidates")
    cpvs = [cpv for cpv, *_ in raw]
    candidates = []
    for _ in range(r.count()):
        fields = [s() for _ in range(5)]
        lists = [tuple(strings[i] for i in r.ids(nstrings)) for _ in range(4)]
        errors = tuple((s(), s()) for _ in range(r.count()))
        deps = _read_trees(r, s, count, cpvs)
        required_use = tuple(strings[i] for i in r.ids(nstrings))
        empty_groups_true = bool(r.varint(2))
        tokens = tuple(
            tuple(strings[i] for i in r.ids(nstrings)) for _ in range(len(DEP_KINDS))
        )
        candidates.append(
            Candidate(
                *fields, *lists, errors, deps, required_use, empty_groups_true, tokens
            )
        )
    r.done()

    listed = []
    for section, name in (
        (SECTION_REPOSITORY, "repository"),
        (SECTION_REQUESTED, "requested"),
    ):
        r = _Reader(sections[section], name)
        listed.append(tuple(strings[i] for i in r.ids(nstrings)))
        r.done()
    r = _Reader(sections[SECTION_USE_EXPAND], "use_expand")
    listed.extend(tuple(strings[i] for i in r.ids(nstrings)) for _ in range(2))
    r.done()

    packages = []
    for cpv, source, eapi, errors, deps, possible, weighed in raw:
        visible, masked, vdb_masked, target, rebuild, *mask = weighed
        if target > len(candidates):
            raise StoreError(f"dependencies: {cpv}'s target {target} out of range")
        packages.append(
            Dependencies(
                cpv,
                source,
                eapi,
                errors,
                _named(deps, cpvs),
                tuple(
                    Possible(kind, atom, choice, tuple(cpvs[i] for i in matches), flags)
                    for kind, atom, choice, matches, flags in possible
                ),
                visible,
                masked,
                vdb_masked,
                (
                    (candidates[target - 1].cpv, candidates[target - 1].repo)
                    if target
                    else None
                ),
                rebuild,
                *mask,
            )
        )
    return meta, inputs, EvaluatedLayer(packages, candidates, *listed)


def decode_repository(data):
    """(RepositoryMeta, Inputs, RepositoryIndex) from repository index bytes; raises
    StoreError."""
    sections = _sections(
        data, REPOSITORY_MAGIC, REPOSITORY_FORMAT_VERSION, REPOSITORY_SECTIONS
    )

    r = _Reader(sections[SECTION_META], "meta")
    meta = RepositoryMeta(r.text(), r.text(), r.text(), r.varint())
    r.done()

    inputs = _read_inputs(sections[SECTION_INPUTS])
    strings = _read_strings(sections[SECTION_STRINGS])
    nstrings = len(strings)

    def reader(section, name):
        r = _Reader(sections[section], name)

        def s():
            return strings[r.varint(nstrings)]

        def listed():
            return tuple(strings[i] for i in r.ids(nstrings))

        def entries():
            return tuple(Entry(s(), listed()) for _ in range(r.count()))

        return r, s, listed, entries

    r, s, _, _ = reader(SECTION_REPOSITORIES, "repositories")
    repositories = tuple(
        Repository(s(), s(), bool(r.varint(2))) for _ in range(r.count())
    )
    r.done()

    r, s, listed, _ = reader(SECTION_VERSIONS, "versions")
    versions = []
    for _ in range(r.count()):
        cp, cpv, slot, sub_slot, eapi = (s() for _ in range(5))
        repo = repositories[r.varint(len(repositories))].name
        keywords, license_tokens, properties, restrict, use = (
            listed() for _ in range(5)
        )
        versions.append(
            Version(
                cp,
                cpv,
                repo,
                slot,
                sub_slot,
                eapi,
                keywords,
                license_tokens,
                properties,
                restrict,
                use,
                s(),
                s(),
                listed(),
            )
        )
    r.done()

    r, s, listed, entries = reader(SECTION_VISIBILITY, "visibility")
    eapis = tuple(
        Eapi(s(), bool(r.varint(2)), bool(r.varint(2))) for _ in range(r.count())
    )
    accept_keywords, environment_keywords, arch = listed(), listed(), s()
    profile_keywords, profile_accept_keywords = (
        tuple(entries() for _ in range(r.count())) for _ in range(2)
    )
    accept_keywords_entries = entries()
    masks, unmasks = listed(), listed()
    accepted = []
    for _ in range(3):
        accepted.append(listed())
        accepted.append(entries())
    r.done()
    visibility = Visibility(
        eapis,
        accept_keywords,
        environment_keywords,
        arch,
        profile_keywords,
        profile_accept_keywords,
        accept_keywords_entries,
        masks,
        unmasks,
        *accepted,
    )

    r, s, listed, _ = reader(SECTION_ADVISORIES, "advisories")
    advisories = []
    for _ in range(r.count()):
        nr, title, synopsis = s(), s(), s()
        revision = r.varint()
        packages = tuple(
            AdvisoryPackage(s(), s(), listed(), listed()) for _ in range(r.count())
        )
        advisories.append(Advisory(nr, title, synopsis, revision, packages))
    r.done()
    return (
        meta,
        inputs,
        RepositoryIndex(repositories, tuple(versions), visibility, tuple(advisories)),
    )


def write(path, data):
    """Replace path with data so readers see the old store or the new one, never a mix."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=f".{os.path.basename(path)}.", dir=directory)
    try:
        with os.fdopen(fd, "wb") as f:
            os.fchmod(f.fileno(), 0o644)
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp, path)
    except BaseException:
        os.unlink(temp)
        raise
    dir_fd = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)
