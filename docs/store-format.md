# Store format

Status: format version 2, implemented by `builder/egraph_build/store.py` (writer and a Python
reader) and `src/store.cpp` (C++ reader). Any layout change bumps the version.

## Requirements

- One file. No directory trees, no database dependency.
- Parse in milliseconds at real size (about 2.3k packages and 36k atoms on the dev box). If a
  plain-file encoding meets that, no database is used.
- Decodable in C++ with bounds-checked reads over `std::span<const std::byte>`: no struct overlays,
  no `mmap`, fixed little-endian, lengths before data.
- Versioned: a format version that both sides check, with any mismatch treated as stale.
- Atomic replace: write to a temp file in the same directory, fsync, rename.

## Logical content

1. **Header.** Magic, format version, producer versions (egraph, portage), EROOT, build time,
   and the profile's implicit IUSE settings that USE-dep matching needs (`design.md`).
2. **Inputs.** `(path, kind, mtime_ns, size)` for every file and directory the builder read.
   Freshness is exactly "every input still stats the same".
3. **Strings.** An interned table; package data refers to strings by index.
4. **Packages.** cpv, cp, slot, sub-slot, repo, USE, IUSE, EAPI, parse errors.
5. **Dependency trees.** Per package and per kind, a flat node list with parent indices. Node types
   are atom, any-of (`||`), all-of (a group inside `||`), and blocker (weak or strong).
6. **Resolved edges.** Per atom node, the installed cpvs that satisfy it with USE deps honored.
   Empty means unsatisfied. Per any-of node, which alternatives are satisfied.
7. **Sonames.** Per package, provides and requires with multilib category, plus the resolved
   requires-to-provider edges.
8. **Roots.** World atoms, world sets, @system and @profile atoms, each with the packages they match.

## Encoding

Binary sections. Chosen over line-oriented text for a 1.6x cheaper C++ decode, a 40% smaller
file, and a section table that lets the freshness check skip everything but the inputs.
Debugging goes through `egraph export --json` and `egraph-build --json`.

### Framing

- Fixed-width integers are little-endian. Offsets are from the start of the file.
- Header, 16 bytes: magic `EGRAPH\0\0`, `u32` format version, `u32` section count.
- Section table, 20 bytes per entry: `u32` id, `u64` offset, `u64` length. Each id appears at
  most once and every section lies inside the file. An unknown id is a format mismatch: layout
  changes bump the version rather than add optional sections.
- Everything inside a section is a varint: unsigned LEB128, at most 10 bytes, the tenth holding
  at most one bit. A value read into a 32-bit field must fit.
- Every list is a varint count followed by its elements. Each element takes at least one byte, so
  a reader rejects any count larger than the bytes left in the section before allocating.
- String, package and node references are indices, checked against their table.
- A section must be consumed exactly; trailing bytes are an error.
- Any violation rejects the whole store, which is then treated as stale.

### Sections

| Id | Section | Contents |
|---|---|---|
| 1 | Meta | egraph version, portage version, EROOT (length-prefixed), build start in ns |
| 2 | Inputs | count, then `(path, kind, mtime_ns, size)`; path length-prefixed |
| 3 | Strings | count, then length-prefixed bytes; string 0 is empty |
| 4 | Packages | count, then the records below |
| 5 | Roots | count, then `(atom, matches)`; written empty until roadmap step 7 |

All five sections are required.

Inputs come from `lstat`. Kind is 0 file, 1 directory, 2 symlink, 3 missing (mtime and size 0).
An mtime before 1970 is clamped to 0 on both sides. The build start is taken before any input is
stat'ed; an input whose mtime is within 1 s before it is racy and never trusted (`design.md`).

Meta and inputs carry their strings inline, so freshness reads sections 1 and 2 and nothing else.
Input paths are unique, so interning them would save nothing.

Strings are bytes as portage returned them (UTF-8 in practice, not validated). The builder
encodes with `surrogateescape`, so any str portage decoded round-trips, and the JSON exports on
both sides spell an undecodable byte as Python does (`\udcXX`).

### Package record

In package order, which is sorted by cpv:

1. String ids: cpv, cp, slot, sub-slot, repo, EAPI.
2. USE and IUSE: lists of string ids.
3. Errors: list of `(kind, message)` string ids, for dependency strings portage could not parse.
4. For each kind in the order BDEPEND, DEPEND, IDEPEND, PDEPEND, RDEPEND, a node list. A node is
   `type, parent, atom, matches`:
   - type: 0 atom, 1 any-of (`||`), 2 all-of (a group inside `||`), 3 weak blocker, 4 strong
     blocker.
   - parent: 0 for a top-level node, otherwise 1 + the parent's index in this list, which must
     be an earlier any-of or all-of node.
   - atom: string id of the atom as portage prints it after USE reduction; 0 for groups and
     only for groups.
   - matches: package ids the atom matches with USE deps honored (for a blocker, what it blocks).
     Empty for groups: satisfaction of `||` follows from the children.
5. Provides: list of `(multilib category, soname)` string ids.
6. Requires: list of `(multilib category, soname, providers)`, providers a list of package ids.
