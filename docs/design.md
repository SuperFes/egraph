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

## Roots and exit codes

- Both tools take `--root` and `--config-root`, defaulting to `ROOT` and `PORTAGE_CONFIGROOT`,
  and `egraph` passes them to `egraph-build`. The store records its EROOT and is stale for any
  other.
- Both tools share exit codes (0 ok, 1 failure, 2 usage, 3 not implemented); a test pins
  `builder/egraph_build/cli.py` to the `Exit` enum in `src/cli.hpp`.

## Freshness: validate on read

The store carries its own input list, like a make `.d` file: every file and directory the builder
read, with `st_mtime_ns` and size. That covers each `/var/db/pkg/<cat>/<pf>` directory (in-place
changes), each category directory (packages added or removed), the world file, and the profile
files.

On load `egraph` stats every input (a few thousand `stat` calls, a few ms):

- Nothing changed: answer from the store.
- Something changed: run `egraph-build --incremental`, which re-evaluates only the changed
  packages, then reload. `--no-refresh` answers from the stale store with a warning.
- The store cannot be written (unprivileged user): see open questions.

No merge-time hook is needed, and edits to the vdb made outside portage are caught too.
`/etc/portage/postsync.d` can prebuild the later layers after a sync.

## Rebuild and check

- `egraph rebuild`: full rebuild via `egraph-build --full`.
- `egraph check`: build a fresh store in memory and diff it against the stored one; nonzero exit on
  drift. This is the recovery tool and the standing proof that incremental refresh is exact.
- `EGRAPH_STRICT=1`: every refresh also runs a full build and fails on any difference. Used by tests
  and while shaking out new layers.

## Store location

One file, no directory trees: `${EROOT}/var/cache/egraph/installed.egraph` by default, overridable
with `--store` or `EGRAPH_STORE`. Written as a temp file plus rename, so readers never see a
partial store.

## Consumers

Each consumer runs in shadow mode against the tool it replaces before anyone relies on it.

- CLI queries: `deps`, `rdeps`, `why`, `soname`, `broken`, `orphans`, `export`, `stats`.
- The portage fork's neighborhood completion (`depgraph._installed_graph()`), through a small Python
  reader, which removes the 0.37 s index build from every emerge run.
- The graph viewer (`~/.local/bin/portage-graph-view`) reading the store instead of building its own.
- The update fast path, once the candidate layer exists: `-uDN @world` becomes a set difference,
  and an empty difference means nothing to resolve.

## Open questions

- **Unprivileged refresh:** proposed fallback is a store in `$XDG_CACHE_HOME/egraph/` when the
  system store is stale and unwritable, so non-root queries are never wrong.
- **Store for the portage fork:** a Python reader in `egraph_build`, or the fork calls `egraph
  export`. Decide when that consumer is next.
