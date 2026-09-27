"""The store file: encoding, decoding and atomic replacement (docs/store-format.md)."""

import os
import struct
import tempfile
from typing import NamedTuple

from egraph_build.installed import InstalledLayer, Node, Package
from egraph_build.model import DEP_KINDS
from egraph_build.profile import ImplicitIuse, has_iuse_effective
from egraph_build.roots import Root

MAGIC = b"EGRAPH\0\0"
FORMAT_VERSION = 4
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


class Input(NamedTuple):
    path: str
    kind: int
    mtime_ns: int
    size: int


def default_path(eroot):
    return os.path.join(eroot, DEFAULT_PATH)


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

    w = _Writer()
    w.varint(len(inputs))
    for item in inputs:
        w.text(item.path)
        w.varint(item.kind)
        w.varint(item.mtime_ns)
        w.varint(item.size)
    sections[SECTION_INPUTS] = w.out

    w = _Writer()
    w.varint(len(packages))
    for pkg in packages:
        for value in (pkg.cpv, pkg.cp, pkg.slot, pkg.sub_slot, pkg.repo, pkg.eapi):
            w.varint(strings(value))
        w.varint(1 if has_iuse_effective(pkg.eapi) else 0)
        w.ids([strings(flag) for flag in pkg.use])
        w.ids([strings(flag) for flag in pkg.iuse])
        w.varint(len(pkg.errors))
        for key, message in pkg.errors:
            w.varint(strings(key))
            w.varint(strings(message))
        for nodes in pkg.deps:
            w.varint(len(nodes))
            for node in nodes:
                w.varint(node.type)
                w.varint(node.parent + 1)
                w.varint(strings(node.atom))
                w.ids([index[cpv] for cpv in node.matches])
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
        w.ids([index[cpv] for cpv in root.matches])
    sections[SECTION_ROOTS] = w.out

    w = _Writer()
    w.varint(len(strings.table))
    for value in strings.table:
        w.text(value)
    sections[SECTION_STRINGS] = w.out

    w = _Writer()
    for flags in meta.implicit:
        w.varint(len(flags))
        for flag in flags:
            w.text(flag)
    sections[SECTION_PROFILE] = w.out

    offset = _HEADER.size + _ENTRY.size * len(sections)
    table = bytearray()
    body = bytearray()
    for section_id in SECTIONS:
        data = sections[section_id]
        table += _ENTRY.pack(section_id, offset + len(body), len(data))
        body += data
    return _HEADER.pack(MAGIC, FORMAT_VERSION, len(sections)) + table + body


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


def _sections(data):
    if len(data) < _HEADER.size:
        raise StoreError("truncated header")
    magic, version, count = _HEADER.unpack_from(data)
    if magic != MAGIC:
        raise StoreError("not an egraph store")
    if version != FORMAT_VERSION:
        raise StoreError(f"format version {version}, expected {FORMAT_VERSION}")
    if count != len(SECTIONS) or len(data) < _HEADER.size + _ENTRY.size * count:
        raise StoreError("bad section table")
    found = {}
    for i in range(count):
        section_id, offset, length = _ENTRY.unpack_from(
            data, _HEADER.size + _ENTRY.size * i
        )
        if section_id not in SECTIONS or section_id in found:
            raise StoreError(f"unexpected section {section_id}")
        if offset > len(data) or length > len(data) - offset:
            raise StoreError(f"section {section_id} outside the file")
        found[section_id] = memoryview(data)[offset : offset + length]
    return found


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

    r = _Reader(sections[SECTION_INPUTS], "inputs")
    inputs = tuple(
        Input(r.text(), r.varint(), r.varint(), r.varint()) for _ in range(r.count())
    )
    r.done()

    r = _Reader(sections[SECTION_STRINGS], "strings")
    strings = [r.text() for _ in range(r.count())]
    r.done()

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
        use = tuple(strings[i] for i in r.ids(nstrings))
        iuse = tuple(strings[i] for i in r.ids(nstrings))
        errors = tuple((s(), s()) for _ in range(r.count()))
        deps = []
        for _ in DEP_KINDS:
            nodes = []
            for index in range(r.count()):
                node_type = r.varint(5)
                parent = r.varint(index + 1) - 1
                if parent >= 0 and nodes[parent].type not in (1, 2):
                    r.fail("parent is not a group")
                nodes.append(Node(node_type, parent, s(), r.ids(count)))
            deps.append(nodes)
        provides = tuple((s(), s()) for _ in range(r.count()))
        requires = []
        for _ in range(r.count()):
            requires.append((s(), s()))
            r.ids(count)
        raw.append((fields, use, iuse, errors, deps, provides, tuple(requires)))
    r.done()

    r = _Reader(sections[SECTION_ROOTS], "roots")
    raw_roots = []
    for _ in range(r.count()):
        raw_roots.append((s(), s(), r.ids(count)))
    r.done()

    cpvs = [fields[0] for fields, *_ in raw]
    packages = []
    for fields, use, iuse, errors, deps, provides, requires in raw:
        packages.append(
            Package(
                *fields,
                use=use,
                iuse=iuse,
                errors=errors,
                deps=tuple(
                    tuple(
                        n._replace(matches=tuple(cpvs[i] for i in n.matches))
                        for n in nodes
                    )
                    for nodes in deps
                ),
                provides=provides,
                requires=requires,
            )
        )
    roots = tuple(
        Root(name, atom, tuple(cpvs[i] for i in matches))
        for name, atom, matches in raw_roots
    )
    return meta, inputs, InstalledLayer(packages, roots)


def write(path, data):
    """Replace path with data so readers see the old store or the new one, never a mix."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=".installed.", dir=directory)
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
