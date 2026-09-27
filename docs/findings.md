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
