# Notes for upstream portage

A record of what upstream could take, with evidence. Nothing here goes upstream until it is
stable and proven.

## Portage fork commits

The fork is `/Development/Gentoo/portage`, branch `depgraph-trail-shadow`, on top of `77f10d9bf`.
Each commit passes the resolver suite on its own.

| Commit | Change | Opt-in |
|---|---|---|
| `ea7f88320` | depgraph: memoize resolver queries by graph revision | env var |
| `7971ef8db` | depgraph: skip slot-operator update probes without a candidate | default |
| `0be0f70a7` | config: replay `regenerate()` for packages with identical inputs | env var |
| `a88f6fe47` | resolver: persist per-package USE and mask facts across runs | env var |
| `b0ac9d838` | dbapi: add an index of installed package dependencies | library |
| `d16e54a97` | depgraph: complete the graph from the installed packages a change affects | `PORTAGE_DEPGRAPH_NEIGHBORHOOD` |

## Ideas egraph proves out

- A persisted installed graph with stat-based freshness, which removes the per-run vdb walk.
- Dependency kinds kept as separate graphs, which removes the manufactured cycle (`findings.md`).

## Divergences from portage

Record each case where egraph's answer differs from portage's, with the test that proves which
is right.

None yet.
