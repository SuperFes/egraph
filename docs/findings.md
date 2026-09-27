# Findings

These measurements came from the portage fork work (September 2026). Add new ones here with the
date and how they were measured.

## Why portage is slow at this

- Portage keeps no reverse index of installed packages. Complete mode finds the installed reverse
  dependencies a change might break by walking forward over every installed package.
- Case: `emerge -upv @world` with one upgrade (networkmanager), 7 backtracking attempts:
  - Before completion the graph has 291 nodes and builds in about 10 ms.
  - `_complete_graph` grows it to 2,327 nodes with 48k `_add_pkg` calls, about 2.5 s per attempt.
  - That runs in 3 of the 7 attempts, about 70% of the whole run.
- The real neighborhood of that change is 7 packages and 14 edges.
- Portage's startup floor (`emerge --version`) is 0.65 s: imports plus config load. egraph's query
  path avoids it by never importing portage.

## Installed index (fork `_InstalledGraph.py`)

- Reading the vdb uncached takes about 0.3 s and yields about 36k atoms. The full index builds in
  0.37 s; a neighborhood query takes 0.3 ms.
- Installed packages have fixed USE, so dependency strings reduce with no decisions. `||` groups
  were kept as choices, which makes that index a superset.
- Neighborhood completion in the fork, measured in instructions:

  | `-uv @world` | Instructions |
  |---|---|
  | Baseline | 95.2G |
  | With neighborhood completion | 30.7G |
  | Plus memo and facts | 21.1G |

  It showed 0 differences on the strict resolver suite (467 tests) and 10 real commands in shadow
  mode.

## Graph shape

Superset cp-graph from md5-cache (every version and every USE branch unioned, zero decisions):

- 38,867 nodes and 176,997 edges.
- 92.3% of nodes lie in no cycle.
- The largest SCC has 2,686 nodes.

On the real @world closure (11,387 packages), the cycle comes from merging dependency kinds:

| Graph | Acyclic | Largest SCC |
|---|---|---|
| All kinds merged | 77.3% | 2,586 |
| RDEPEND only | 95.2% | 526 |
| DEPEND only | 96.7% | 324 |
| BDEPEND only | 97.4% | 253 |

Hence egraph never merges kinds.

Blockers:

- A weak blocker (`!`) appears in 7.7% of ebuilds and a strong one (`!!`) in 0.3%.
- 6.0% of the closure carries one.
- Blockers are constraints, not edges.

## Where portage's resolver time goes

This shapes the later layers.

- Slot-operator verification took about half of `-uDN` time and found nothing in about 28k probes.
- First-touch package facts (`config.setcpv`, about 1.1 to 1.25 ms each across 6.3k cpvs) took
  15 to 24%. That is the work the evaluated layer would precompute.
- USE is not a search variable: profile, package.use and IUSE determine it. Only autounmask
  searches it.

## Benchmarking traps on this box

- Wall clock varies by ±25% for identical runs, and the load average often sits above 20. Use
  `perf stat -x, -e instructions:u,cycles:u` and interleave A/B runs.
- Compare cycles too: fewer instructions can still lose on IPC through memory growth.
- Gate a change on identical output before scoring it as a speedup.
- For builder timings, make sure the intended portage is loaded: print
  `os.path.dirname(portage.__file__)`.

## Store encoding (2026-09-27, roadmap step 1)

Measured on the live vdb (2,324 packages) with `perf stat -x, -e instructions:u,cycles:u`, as the
median of interleaved runs (7 for the builder, 21 for C++ loads, 9 for Python loads), warm page
cache, unprivileged user. Prototypes were throwaway scripts; the method is below so it can be
repeated.

### Builder cost split

One script run to a cutoff, so each row adds one phase to the one above.

| Phase | Δ instructions | Δ cycles |
|---|---|---|
| Import portage, load config | 0.80G | 0.49G |
| `aux_get` of 12 keys for every package | 1.93G | 0.71G |
| `use_reduce` into Atoms, parse sonames | 1.46G | 0.64G |
| `vardb.match` for every non-blocker atom (USE deps honored) | 3.96G | 1.57G |
| Total | 8.14G | 3.41G |

- 36,043 atoms after USE reduction, but only 5,679 distinct: interning atom strings pays, and
  vardb's per-atom match cache is why matching is not worse.
- Matching is half the build. An atom's matches can only change when a package of its cp is
  added, removed or changed, so incremental refresh should re-match only those atoms.

### Logical content at real size

22,891 strings, 2,431 inputs (category and package directories plus the world file), 2,324
packages, 36,975 dependency tree nodes (atoms, blockers, any-of and all-of groups), 34,647
resolved matches, 209 world atoms.

### Encodings

Both prototypes hold the same content and their C++ loaders decode to the same model (checked
by checksum). The loaders follow the project rules: bounds-checked reads over
`std::span<const std::byte>` or `std::string_view`, a file read into one buffer, strings pooled
in one `std::string`, lists in shared index vectors.

| | A: binary sections, LEB128 | B: tab-separated lines |
|---|---|---|
| Size | 1.17 MB | 1.96 MB |
| Size, zstd -19 | 307 KB | 293 KB |
| C++ decode, net of process and file read | 30M ins, 8.5M cycles | 50M ins, 13.5M cycles |
| Pure-Python decode, net of interpreter start | 750M ins, 157M cycles | 714M ins, 179M cycles |

- Both parse in a few milliseconds from C++; A is 1.6x cheaper and 40% smaller.
- In Python both take about 40 ms, versus 0.37 s for the fork's index build, so the encoding
  does not decide the Python reader question. The Python numbers are lower bounds: they
  tokenize the whole file but do not rebuild the records.
- A's section table lets the freshness check decode the header and inputs only.

Decision: A. See `store-format.md`.

## Installed layer (2026-09-27, roadmap step 2)

- `egraph-build --json` on the live vdb: 9.31G instructions (median of 7 interleaved runs),
  against 8.15G for the step 1 phases up to matching. Building the trees and the reverse index
  and writing 5.7 MB of JSON cost about 1.1G. Cycles were not comparable: the load average sat
  above 20 during the runs.
- Agreement with portage on the live vdb, every subject, no sampling: `installed`, `errors`,
  `matches` (5,679 distinct atoms), `deps` (2,324 packages), `rdeps` (34,432 edges),
  `soname_providers` and `soname_consumers` (1,001 sonames, 15,177 uses). Zero differences.
- The unsampled oracle is too slow to run routinely: `rdeps` and the soname queries rescan every
  package per subject, and the unsampled pytest run passed 10 minutes before reaching
  `soname_consumers`. The full check above inverted the oracle's per-package answers once
  instead, which is equivalent for these queries.
- use_reduce shapes the trees in ways worth knowing: a `||` left with one alternative becomes a
  plain atom, nested `||` groups are flattened, and in EAPI 7+ a `||` emptied by USE becomes the
  never-matching atom `__const__/empty-any-of` (older EAPIs drop it).
