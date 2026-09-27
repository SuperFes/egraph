# Store format

Status: draft. The logical content is settled; the encoding waits on roadmap step 1.

## Requirements

- One file. No directory trees, no database dependency.
- Parse in milliseconds at real size (about 2.3k packages and 36k atoms on the dev box). If a
  plain-file encoding meets that, no database is used.
- Decodable in C++ with bounds-checked reads over `std::span<const std::byte>`: no struct overlays,
  no `mmap`, fixed little-endian, lengths before data.
- Versioned: a format version that both sides check, with any mismatch treated as stale.
- Atomic replace: write to a temp file in the same directory, fsync, rename.

## Logical content

1. **Header.** Magic, format version, producer versions (egraph, portage), EROOT, build time.
2. **Inputs.** `(path, kind, mtime_ns, size)` for every file and directory the builder read.
   Freshness is exactly "every input still stats the same".
3. **Strings.** An interned table; everything else refers to strings by index.
4. **Packages.** cpv, cp, slot, sub-slot, repo, USE, IUSE, EAPI, parse errors.
5. **Dependency trees.** Per package and per kind, a flat node list with parent indices. Node types
   are atom, any-of (`||`), all-of (a group inside `||`), and blocker (weak or strong).
6. **Resolved edges.** Per atom node, the installed cpvs that satisfy it with USE deps honored.
   Empty means unsatisfied. Per any-of node, which alternatives are satisfied.
7. **Sonames.** Per package, provides and requires with multilib category, plus the resolved
   requires-to-provider edges.
8. **Roots.** World atoms, world sets, @system and @profile atoms, each with the packages they match.

## Candidate encodings

- **A. Binary sections:** a header, a section table of `(id, offset, length)`, then the sections
  above, using varint lengths and indices. Smallest and fastest to load; needs `egraph export
  --json` for debugging.
- **B. Line-oriented text:** one record per line, tab-separated fields, string table first.
  Greppable and diffable, but larger and slower to parse.

Step 1 prototypes both at real size and measures load time. Leaning A.
