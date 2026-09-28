# Design

## Goal

An always-available graph of the installed system that any tool can query in milliseconds, and
that later grows the config-evaluated and candidate layers needed for fast update resolution.

## Why a separate tool

Building inside portage means changing portage's hot paths and carrying the patches. As a separate
tool, egraph owns everything on the query path and treats portage as two things only:

1. **The evaluator** for building the store: USE, profile stacking and dep reduction, where
   reimplementation is years of work and the main correctness risk.
2. **The oracle**: any piece egraph later implements itself (for example reading vdb entries in C++)
   runs against portage in strict mode until they agree.

pkgcore was considered and rejected: its semantics can drift from what emerge does, and it offers
nothing on the query path.

## Layers

Each layer has its own inputs and invalidation, and is proven independently.

| Layer | Contents | Depends on | Status |
|---|---|---|---|
| Installed | installed packages, exact dependency edges, sonames, blockers, roots | `/var/db/pkg`, world file, profile (for @system) | first |
| Evaluated | effective USE, visibility, reduced deps of repo packages | repo metadata, `/etc/portage`, profile, env | later |
| Candidate | best visible version per cp, pending updates and rebuilds | both of the above | later |

The installed layer needs no resolver decisions: installed packages have fixed USE, so every
conditional reduces to plain atoms. Only `||` groups remain, and on an installed system each group
records which alternatives are actually satisfied.

## Installed layer semantics

Starts from the fork's `_InstalledGraph.py` and extends it where a shared utility needs exact
answers rather than a superset:

- **Exact matching.** Atoms match with USE dependencies against the child's installed USE and IUSE,
  honoring `(+)`/`(-)` defaults. The portage index ignored USE deps because it only needed a
  superset.
- **Choice nodes.** A `||` group is a node with ordered alternatives (atoms or all-of groups), each
  marked satisfied or not. The group is satisfied if any alternative is.
- **Unsatisfied edges are kept.** An atom nothing installed satisfies is a first-class result, which
  is what `egraph broken` reports.
- **Kinds stay separate.** BDEPEND, DEPEND, RDEPEND, PDEPEND and IDEPEND edges are never merged.
  Each kind alone is 95-97% acyclic; merging them manufactures a 2,586-node cycle (findings).
- **Blockers are constraints, not edges.** They are stored with their strength and what they match,
  and are never traversed as dependencies.
- **Sonames.** `REQUIRES` to `PROVIDES` edges, per multilib category.
- **Roots.** The world file and `world_sets` are read directly. @system and @profile need profile
  stacking, so the builder evaluates them and records the profile files it consulted.

## Queries take portage atoms

Every query that names packages accepts a full portage atom (versions, slots and sub-slots, USE
dependencies with defaults, repository) and means by it exactly what `vardb.match` means.

- Atoms are matched in C++ (`src/atom.cpp`, `src/version.cpp`) against the stored package data,
  porting `match_from_list` and vardb's USE-dependency check rather than PMS prose: portage's
  `vercmp` (a missing component sorts below 0, so 1.0.0 > 1.0), `=...*` globs on version-part
  boundaries, `~` comparing version text without the revision.
- USE-dep defaults on installed packages depend on the profile's implicit IUSE, so the store
  carries it (`IUSE_EFFECTIVE` for EAPI 5+, whose built packages also count every flag in their
  USE; literal flags and `x_.*` prefixes for older EAPIs), and the profile files are inputs.
- Query arguments cannot be blockers or conditional USE dependencies (`[x?]`, `[x=]`): both
  only mean something next to a parent package. An exact cpv is also accepted as a convenience.
- The matcher runs in shadow against `vardb.match` over a generated corpus of 339 atoms, every
  atom in every scenario, and all 5,667 distinct atoms on the dev box, with no differences.
  Portage is right until a test says otherwise.

## Query output

Two layouts of the same records. On a terminal, queries are laid out for people:

- Grouped by the package asked about, aligned, with counts. `deps`/`rdeps` show each dependency
  once with the kinds it appears under as a letter matrix (`R··D·`: R runtime, I install, P post,
  D build, B build host), `why` is a chain from its root set, `broken` uses the same letters, and
  a legend follows any layout that uses them.
- Coloured part by part (category, name, version; operator, slot, repository, USE flags), in
  truecolor when `COLORTERM` says so and xterm-256 otherwise. `--color auto|always|never`, and
  `NO_COLOR` is honoured.
- Decorated with Nerd Font icons by default; `--glyphs unicode` or `ascii` (or `EGRAPH_GLYPHS`)
  for fonts without them.

Piped, or with `--layout lines` (also `EGRAPH_LAYOUT`), the output is one tab-separated record
per line, sorted and without duplicates, so it greps, diffs and compares against the oracle line
for line. The human layouts (`src/human.cpp`) are built from those records and nothing else, so
both carry the same information and the oracle tests cover both.

The records are:

- `deps` / `rdeps PKG...`: `parent kind atom child`, plus `any-of` for an alternative inside a
  `||` group. `PKG` is a portage atom (or an exact cpv).
- `soname NAME [--providers]`: `cpv multilib-category`.
- `broken`: `cpv kind dependency`, one per top-level dependency nothing installed satisfies, with
  `||` groups rendered as portage's `paren_enclose` does.
- `export [PKG...] [--depth N] [--direction reverse|forward|both]`: the packages within N
  dependency edges of PKG (all packages when none are given), as the canonical JSON or as DOT
  with every edge between them. Blockers are not followed.
- `orphans [--with-bdeps y|n]`: one cpv per line, what `emerge --depclean` would remove. When
  depclean would refuse (runtime dependencies nothing installed satisfies), the orphans are still
  listed, the unresolved dependencies go to stderr as `cpv kind atom`, and the exit status is 1.
  An empty @world is refused outright, as depclean does.
- `why ATOM [--with-bdeps y|n]`: for each installed package the atom matches, a shortest chain
  of what depclean follows from a root: `@set atom cpv`, then one `deps`-style line per
  dependency, chains separated by a blank line. Ties prefer the earlier root, then runtime
  dependencies over build-time ones. A package depclean would remove gets a message on stderr and
  exit status 1.

## Terminal interface

`egraph tui` (roadmap 10b) is built when Notcurses is found (`-Dtui=auto`, the default). The
Notcurses C API is confined to `src/screen.cpp` behind the value-typed `Screen` (keys, pens,
cells), as `src/os.cpp` confines the OS; the app in `src/tui.hpp` is a template over the screen,
so it is tested with a fake one and needs no terminal. It shares the human layout's palette and
glyphs (`--glyphs`).

## Orphans: emulating depclean

`orphans` is depclean's graph completion replayed over the store (`src/depclean.cpp`), not plain
reachability, because depclean is choosier than reachability in ways users rely on:

- Roots are @selected, @system and @profile. Every atom, whether a root or a dependency, keeps
  only the highest installed package it matches: an unslotted world atom does not keep old
  kernel slots.
- All five dependency kinds are followed (`--with-bdeps=n` drops DEPEND and BDEPEND); sonames
  and blockers are not, as with depclean's default `--ignore-soname-deps=y`.
- `||` groups and `virtual/*` atoms are resolved after the plain dependencies of every queued
  package, in emerge's stack order, with `dep_zapdeps`'s preferences: an alternative whose atoms
  are all installed, promoted ahead of earlier ones when it is already kept or selects a higher
  version of the same cp; then one with unmet USE dependencies; then one with some atoms
  installed. So `|| ( a b )` keeps `a` when neither is kept yet, and only `b` when `b` already
  is.
- Unresolved runtime dependencies (RDEPEND, PDEPEND, IDEPEND) make depclean refuse; unresolved
  build-time ones do not.

`why` walks the same traversal. It answers with the dependency that keeps a package, not with
every root atom that happens to match it: an unslotted world atom matching two slots keeps only
the higher one, and the other is explained by whatever depends on it.

Dependencies are the ones the vdb recorded at merge time, as `emerge --dynamic-deps=n` reads
them. emerge's default re-reads an installed package's dependencies from its ebuild when the same
version is still in the repository; after an ebuild drops a dependency without a revision bump,
depclean can remove what egraph keeps. On the dev box the two agree, although the vdb carries
about 1,400 `>=sys-libs/glibc-*` runtime dependencies no ebuild states (only @system glibc is
affected, so no answer changes). Offering dynamic dependencies needs the repository's metadata:
the evaluated layer.

In removal mode `dep_zapdeps` asks which packages are *available* (visible: unmasked, or with a
visible equivalent ebuild). egraph has no masking information until the evaluated layer, so it
takes every installed package as visible, which is the usual state of a live system. An
installed package that is masked and gone from the repository can make depclean choose
differently among `||` alternatives. On the dev box both modes agree exactly with depclean.

## Roots and exit codes

- Both tools take `--root`, `--config-root` and `--eprefix`, defaulting to `ROOT`,
  `PORTAGE_CONFIGROOT` and `PORTAGE_OVERRIDE_EPREFIX`, and `egraph` passes the ones given to
  `egraph-build`. The default store is `${ROOT}${EPREFIX}/var/cache/egraph/installed.egraph`.
  The store records its EROOT, and an incremental build for another EROOT is a full one.
- Both tools share exit codes (0 ok, 1 failure, 2 usage, 3 not implemented, 4 drift); a test pins
  `builder/egraph_build/cli.py` to the `Exit` enum in `src/cli.hpp`.

## Freshness: validate on read

The store carries its own input list, like a make `.d` file: every path the builder read, with
its kind (file, directory, symlink, or missing), `st_mtime_ns` and size from `lstat`. That covers
the vdb directory (categories added or removed), each category directory (packages added or
removed), each package directory (in-place changes, which portage writes by rename), and the
configuration that decides how USE dependencies match: `make.globals`, `make.conf`, the
`make.profile` link and every profile directory with its entries, and what the root sets are read
from: the world and world_sets files and portage's set configuration (`sets.conf` files and the
user sets directory). A path recorded as missing is stale once it exists.

Timestamps are coarse, so an input modified within 1 s before the build started is never trusted
as unchanged (the racy-git rule); a store built right after a merge refreshes once more and then
settles.

On load `egraph` stats every input (2,575 `lstat` calls on the dev box, about 2.6 ms):

- Nothing changed: answer from the store.
- Something changed, or the store is missing or corrupt: run `egraph-build --incremental` (the
  command is `--builder` or `EGRAPH_BUILD`), then reload. `--no-refresh` answers from a stale
  store with a warning and fails without one.
- The store cannot be written (unprivileged user): see open questions.

`--incremental` re-reads only the packages that were added or whose directory changed, and in
the others re-matches only the atoms naming a cp that gained, lost or changed a package. Root sets
are read again on every build, so a world or set file edit costs no more than that. A changed
configuration input or another EROOT means a full build. `EGRAPH_STRICT=1` compares
every incremental build against a full one and fails, leaving the old store, on any difference.

No merge-time hook is needed, and edits to the vdb made outside portage are caught too.
`/etc/portage/postsync.d` can prebuild the later layers after a sync.

## Rebuild and check

- `egraph rebuild`: full rebuild via `egraph-build --full`.
- `egraph check`: build a fresh store into a scratch file and diff its packages against the
  stored ones, without refreshing the store first. Prints `+cpv` (missing from the store),
  `-cpv` (no longer installed) or `~cpv` (different) per package and exits 4 on drift. This is
  the recovery tool and the standing proof that incremental refresh is exact.
- `EGRAPH_STRICT=1`: every incremental refresh also runs a full build and fails on any
  difference. Used by tests and while shaking out new layers.

## Store location

One file, no directory trees: `${EROOT}/var/cache/egraph/installed.egraph` by default, overridable
with `--store` or `EGRAPH_STORE`. Written as a temp file plus rename, so readers never see a
partial store.

## Consumers

Each consumer runs in shadow mode against the tool it replaces before anyone relies on it.

- CLI queries: `deps`, `rdeps`, `why`, `soname`, `broken`, `orphans`, `export`, `stats`.
- The portage fork's neighborhood completion (`depgraph._installed_graph()`), by querying `egraph`,
  which removes the 0.37 s index build from every emerge run. The Python store reader decodes the
  live store in about 0.3 s and exists for tests only.
- The graph viewer (`~/.local/bin/portage-graph-view`) reading the store instead of building its own.
- The update fast path, once the candidate layer exists: `-uDN @world` becomes a set difference,
  and an empty difference means nothing to resolve.

## Open questions

- **Unprivileged refresh:** proposed fallback is a store in `$XDG_CACHE_HOME/egraph/` when the
  system store is stale and unwritable, so non-root queries are never wrong.
