# Roadmap

Each step lands as stubs, then tests, then the implementation, and leaves everything working.
Update the status column as steps land.

| # | Step | Status |
|---|---|---|
| 0 | Scaffold | done |
| 1 | Measure and choose the store encoding | done |
| 2 | Installed layer in the builder | done |
| 3 | Store writer and reader | done |
| 4 | Freshness | done |
| 5 | `rebuild` and `check` | done |
| 6 | Queries | 6a done, 6b not started |
| 7 | Roots and orphans | not started |
| 8 | Consumers | not started |
| 9 | Evaluated and candidate layers | not planned yet |

## 0. Scaffold

- The meson project builds an `egraph` binary whose CLI11 subcommands are stubs that exit
  "not implemented".
- A Catch2 harness, and the `builder/egraph_build` package with a pytest harness. `meson test`
  runs both.
- Hardening flags, `-Werror=unsafe-buffer-usage` under clang, and a sanitizer build that passes.
- `.clang-tidy` is clean on the stubs.

## 1. Measure and choose the store encoding

- On the real vdb, split the builder's time into vdb reads, `use_reduce`/atoms, and indexing.
- Prototype encodings A and B from `store-format.md` at real size; time C++ loads with
  `perf stat`.
- Record both in `findings.md`, then settle `store-format.md`.

## 2. Installed layer in the builder

- Port `_InstalledGraph.py` semantics.
- Add exact USE-dep matching, choice nodes, unsatisfied edges and per-kind edges (`design.md`).
- Add a canonical JSON export for tests.
- Tests use `ResolverPlayground` vdbs, including `||`, USE deps with defaults, blockers, sonames
  and slot/sub-slot.
- Done when the strict-xfail comparisons against `egraph_build.oracle` in
  `builder/tests/test_compare.py` and `test_system.py` pass and their markers are removed.

## 3. Store writer and reader

- A Python writer and a C++ reader. A golden test checks that the C++ JSON export equals the
  builder's JSON for the same playground.
- Fuzz the reader with truncated and corrupted stores: it must reject them, never crash (run
  under ASan).

## 4. Freshness

- An input list in the store, and stat-on-load in C++.
- `egraph-build --incremental`, spawned from `src/os.cpp`, and `--no-refresh`.
- Tests: in-place vdb change, package added, package removed, profile edit. `EGRAPH_STRICT`
  checks every refresh against a full build. World edits are tested with roots in step 7, when
  the world file becomes an input.

## 5. `rebuild` and `check`

- `rebuild` forces a full build. `check` diffs the store against a fresh build and exits
  nonzero on drift.

## 6. Queries

- 6a: `deps`, `rdeps`, `soname`, `broken`, `export` (dot, json, neighborhoods) and `stats`, taking
  cpvs and cps. Each is tested through the binary against `egraph_build.oracle` on every
  scenario, and sampled on the live vdb.
- 6b: query arguments are full portage atoms: a C++ atom parser and matcher, run in shadow
  against `vardb.match` until they agree (`design.md`). Needs the profile's implicit IUSE in the
  store.
- `why` needs roots and lands with step 7.

## 7. Roots and orphans

- `why` (shortest path from a root over the reverse graph).

- The builder evaluates @system and @profile and records the profile files as inputs.
- `orphans` runs in shadow against `emerge --depclean --pretend`; differences get tests before
  anyone trusts them.

## 8. Consumers

- The portage fork's `depgraph._installed_graph()` reads the store.
- The graph viewer reads the store.

## 9. Evaluated and candidate layers

- Effective USE, visibility and best visible version, with config fingerprints.
- The `-uDN @world` fast path.
- To be planned once 0-8 hold up.
