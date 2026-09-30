# Store format

Status: format version 4 (evaluated store: 5), implemented by `builder/egraph_build/store.py`
(writer and a Python reader) and `src/store.cpp` and `src/evaluated.cpp` (C++ readers). Any
layout change bumps the version.

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
8. **Roots.** The atoms of @selected (world and world_sets), @system and @profile, nested sets
   expanded, each with the installed packages it matches.

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
| 5 | Roots | count, then `(set, atom, matches)`: set and atom string ids, sets in the order selected, system, profile and atoms sorted within each |
| 6 | Profile | implicit IUSE: `IUSE_EFFECTIVE`, then implicit literal flags, then implicit prefixes, each a count and length-prefixed strings |

All six sections are required.

The profile section is what USE-dependency defaults need to match installed packages outside
portage: a flag counts as in IUSE when the package lists it, or, for an EAPI with
`IUSE_EFFECTIVE`, when it is in that set or in the package's USE, or, for older EAPIs, when it
is one of the literals or starts with one of the prefixes (portage's `x_.*` patterns).

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

1. String ids: cpv, cp, slot, sub-slot, repo, EAPI; then 1 if the EAPI has `IUSE_EFFECTIVE`,
   else 0.
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

## The evaluated store

The installed packages as emerge sees them against the repositories: their dependencies under
`--dynamic-deps=y`, and the versions their cps could move to. It lives beside the installed
store it was built against, named after it (`installed.egraph` → `installed.evaluated.egraph`:
the last extension replaced by `.evaluated.egraph`), and is written by the same builder run.

It uses the installed store's framing with magic `EGRAPHEV` and its own format version, now 7.
Package ids are the installed store's, so an evaluated store is current only while the installed
store beside it has the build start recorded in its meta, and its own inputs stat the same.

| Id | Section | Contents |
|---|---|---|
| 1 | Meta | egraph version, portage version, EROOT (length-prefixed), build start in ns, the installed store's build start in ns |
| 2 | Inputs | as in the installed store |
| 3 | Strings | as in the installed store |
| 4 | Dependencies | count, equal to the installed store's package count, then one record per installed package in its order |
| 5 | Candidates | count, then the records below, sorted by cp, cpv and repo |
| 6 | Repository | list of string ids: every cp with an ebuild in a repository, sorted |
| 7 | Requested | list of string ids: the cps evaluated on request beside those the installed packages reach, sorted |
| 8 | USE_EXPAND | two lists of string ids: the variables of `USE_EXPAND`, then of `USE_EXPAND_HIDDEN`, lowercased, sorted |

All eight sections are required. Section 8 is how emerge groups a package's flags for display:
one group per `USE_EXPAND` variable but the hidden ones, its flags named without the prefix.

A dependency record is how emerge reads the package's dependencies by default, and how it
weighs the package against its repositories under `--update`:

1. String id of the cpv, which must be the installed package's at the same index.
2. Source: 0 ebuild (the same version is in the package's repository and portage supports both
   EAPIs: the ebuild's strings, with the built `:=` atoms appended), 1 vdb (the recorded strings),
   2 moved (the recorded strings with the repositories' package moves applied).
3. EAPI: string id of the EAPI the strings are read with, the ebuild's for source 0.
4. Errors: list of `(kind, message)` string ids, as in the package record.
5. The five node lists, exactly as in the package record, reduced under the installed USE, with
   matches naming installed package ids.
6. Possible: list of what the ebuild would add with flags toggled that the installed build left
   as they are, sorted by kind, atom, choice and flags; empty unless the source is 0, since only
   the ebuild's strings keep their conditionals. Each entry is:
   - kind: index into the dependency kinds (BDEPEND, DEPEND, IDEPEND, PDEPEND, RDEPEND).
   - atom: string id, as portage prints it after reduction under the toggled USE.
   - choice: 1 when the atom is an alternative inside a `||` group, else 0.
   - matches: installed package ids the atom matches.
   - flags: list of string ids, the fewest toggles that add it: `flag` turns a flag on, `-flag`
     turns one off. Only flags in the ebuild's IUSE count, less those the profile masks (to turn
     on) or forces (to turn off).

   Per chain of USE conditionals in a string, the entries are the atoms `use_reduce` selects as
   conditional on the flags the chain needs toggled (its `subset`), under the installed USE with
   them toggled, less the atoms the reduced list holds already. Blockers are left out. An atom
   several chains add keeps each minimal set of flags.
7. Visible: 1 when an ebuild of the same version is visible (the one in the package's
   repository when that has one, else any), as depgraph's `_equiv_ebuild_visible` asks; else 0.
8. Masked: 1 when the package's installed metadata is masked (invalid strings, its CHOST, EAPI,
   keywords, properties, restrictions, `package.mask`, its license), as depgraph's
   `Package.masks` has it under `--dynamic-deps=y`, which reads the EAPI, KEYWORDS and
   dependencies of the same version's ebuild for source 0; else 0. depclean passes over a
   masked package unless it is visible. Only computed where depclean weighs it, for a package
   that is not visible or shares its cp with another installed version; 0 elsewhere.
9. VDB masked: the same under `--dynamic-deps=n`, from the vdb alone.
10. Target: 0, or 1 plus the index of a candidate: the best visible version in the package's
    slot (of one version, the repository of highest priority), when `emerge -u` would replace
    the package with it or `--newuse` would rebuild the package from it. It replaces the package
    when the rebuild list is empty: the candidate is newer, or the package is not visible and
    the candidate is any other version or repository.
11. Rebuild: list of string ids, the flags `--newuse` would rebuild the package for, when the
    target has its version and it is visible, as emerge shows them: `flag*` and `-flag*` changed
    state, `flag%*` and `-flag%` new in IUSE, `(-flag%*)` and `(-flag%)` gone from IUSE, with a
    `*` when the flag's state changed. Those with a `*` are `--changed-use`'s. Flags new in or
    gone from IUSE count only when the profile neither masks nor forces them on the target.

A candidate is one version of a cp in one repository: every visible one, and each masked one
that is installed. The cps are the installed ones, and every cp that emerge could have to pull
in, named in a dependency record's or a visible candidate's node lists: by an atom no installed
package matches, outside any `||` or all-of group installed packages satisfy; or by any
alternative of a `||` group whose satisfied atoms an update could all leave unsatisfied (every
installed package one matches has a visible version in its slot that the atom rejects, USE
aside). The candidates of the cps so reached are followed in turn until nothing new is named.
Blockers name nothing.

1. String ids: cp, cpv, repo, slot, sub-slot.
2. USE: list of string ids, the flags the ebuild would be built with now, within its IUSE.
3. IUSE: list of string ids, without `+` and `-` defaults.
4. Forced: list of string ids, the flags of its IUSE the profile masks or forces on it, which
   emerge shows in parentheses.
5. Reasons: list of string ids, why the version is masked as portage words them (`package.mask`,
   `~amd64 keyword`, `EULA license(s)`); empty when it is visible.
6. Errors: list of `(kind, message)` string ids, as in the package record.
7. The five node lists, as in the package record: the ebuild's dependency strings reduced under
   its USE, with matches naming installed package ids. Empty for a masked candidate.
8. REQUIRED_USE: list of string ids, its tokens in order as `check_required_use` splits the
   string. Empty for a masked candidate and where its EAPI has no REQUIRED_USE; a visible
   candidate's are valid for its EAPI and IUSE, or depgraph would have masked it.
9. Empty groups: 1 when its EAPI holds an empty group satisfied (`empty_groups_always_true`,
   EAPI 6 and older), else 0; 0 for a masked candidate.

Inputs are the installed store's configuration and profile inputs, the user's visibility and USE
configuration (`package.accept_keywords`, `package.mask`, `package.unmask`, `package.license`,
`package.use` and their relatives, `env`, `repos.conf`), and per repository its root, layout,
repository-wide masks, license groups, categories, package moves and eclass directory, every
category directory (for the cps it lists), the metadata cache directory of every category of the
store's cps (installed and candidate), and outside the main repository the package directories
and ebuilds of every such cp it carries.
