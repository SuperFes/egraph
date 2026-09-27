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

- Atoms that already appear in the installed trees resolve from their stored matches.
- Other atoms are matched in C++ against the stored package data. USE-dep defaults on installed
  packages depend on the profile's implicit IUSE (`IUSE_IMPLICIT`, `USE_EXPAND_IMPLICIT`,
  `USE_EXPAND_UNPREFIXED`, `USE_EXPAND_VALUES_*`), so the store carries those, and the profile
  files are inputs.
- The C++ matcher runs in shadow against `vardb.match`, over every atom in the store and a
  generated corpus, before anything relies on it. Portage is right until a test says otherwise.

## Query output

Plain text, one tab-separated record per line, sorted and without duplicates, so the output
greps, diffs and compares against the oracle line for line:

- `deps` / `rdeps PKG...`: `parent kind atom child`, plus `any-of` for an alternative inside a
  `||` group. `PKG` is a cpv or a cp (every installed version) until atoms land (roadmap 6b).
- `soname NAME [--providers]`: `cpv multilib-category`.
- `broken`: `cpv kind dependency`, one per top-level dependency nothing installed satisfies, with
  `||` groups rendered as portage's `paren_enclose` does.
- `export [PKG...] [--depth N] [--direction reverse|forward|both]`: the packages within N
  dependency edges of PKG (all packages when none are given), as the canonical JSON or as DOT
  with every edge between them. Blockers are not followed.

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
`make.profile` link and every profile directory with its entries. A path recorded as missing is
stale once it exists. The world file joins the inputs with roots (roadmap step 7).

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
the others re-matches only the atoms naming a cp that gained, lost or changed a package. A
changed configuration input or another EROOT means a full build. `EGRAPH_STRICT=1` compares
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
