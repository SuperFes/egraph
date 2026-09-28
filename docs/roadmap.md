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
| 6 | Queries | done |
| 7 | Roots and orphans | done |
| 8 | Consumers | done |
| 9 | Evaluated and candidate layers | planned |
| 10 | Output and UX | in progress |
| 11 | Build monitor | done |

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
  store. `egraph match` lists what each atom matches.
- `why` needs roots and lands with step 7.

## 7. Roots and orphans

- 7a (done): the builder records @selected, @system and @profile through portage's set
  configuration, with the world, world_sets and set configuration files as inputs.
- 7b (done): `orphans` replays depclean's graph completion; it agrees with depclean on every
  scenario and on the live system, with and without `--with-bdeps`.
- 7c (done): `why`, a shortest chain from a root through what depclean follows, checked link by
  link against depclean's recorded parents and their shortest distance.

## 8. Consumers

- The portage fork's `depgraph._installed_graph()` reads the store. 8a (done): `egraph
  affected`, one call answering what `_complete_neighborhood` asks of the fork's
  `InstalledGraph`, held to the fork's own answers on every scenario and the live system. 8b
  (done): the fork asks it (`PORTAGE_DEPGRAPH_EGRAPH`), in shadow or strict mode against its
  own index, or instead of it.
- The graph viewer reads the store (done): one `egraph export --format json` in place of the
  fork's index, its edges USE-exact where the index's ignored USE dependencies. After a merge,
  portage's post_emerge hook refreshes the store (`egraph refresh`, installed under
  `/etc/portage`), so no resolution pays for it.

## 9. Evaluated and candidate layers

The repository side, as far as queries about installed packages need it. Measured on the dev box
(8 repos, 21,861 cps, 38,303 ebuilds, warm cache): every installed package is still in its repo,
and 1,648 of the 2,326 have dependencies there that differ from their vdb's, mostly RDEPEND.
Reading those ebuilds' metadata costs 2.2 s, the visible versions of all installed cps 1.9 s
(4,881 cpvs; `bestmatch-visible` 1.1 s), and effective USE 0.5 ms per package.

Decided with the user: options default to portage's own defaults unless there is a compelling
reason (so `--dynamic-deps` defaults to y, as emerge's does); every repository in repos.conf is
covered; `updates` starts with the installed packages, and the aim is for egraph to become a
complete suite, resolution included, eventually.

The builder evaluates everything through portage, as for the installed layer; C++ never
reimplements visibility or USE. The layer is its own store file (`evaluated.egraph`, specified
in `store-format.md`) with its own inputs, so that a sync does not touch the installed store and
a merge refreshes only what it changed.

- 9a (done): oracle first. `oracle.py` answers each new question the slow way: an installed package's
  dynamic dependencies (FakeVartree's rule: the ebuild's dependencies when the same version is
  in its repository and both EAPIs are supported, the built `:=` atoms kept; otherwise the vdb's
  with the repository's package moves applied), its effective USE, whether a cpv is visible and
  why not (keywords, masks, license), and the best visible version per slot. Comparison
  scaffolding with strict xfails, and scenarios with changed ebuilds, masks, keywords and moves.
- 9b (done): store format and builder pass. `installed.evaluated.egraph` beside the installed
  store, written by the same builder run: per installed cpv, its dynamic dependency trees in
  the installed layer's node format, matched against the installed packages, and how they were
  derived (ebuild, vdb, vdb with moves); per installed cp, its visible versions in every
  repository with slot, effective USE and IUSE, and the reason for each masked one that is
  installed. It uses the installed store's package ids and records that store's build start,
  so a rebuild of either makes it stale. Inputs: the configuration and profile files the
  installed layer tracks, the user's visibility and USE configuration, and per repository its
  masks, moves, license groups and the metadata cache directories of installed categories;
  outside the main repository also the installed cps' package directories and ebuilds.
  `egraph export --evaluated` prints it as the builder's `--evaluated-json` does.
- 9c (done): dynamic deps in queries. `--dynamic-deps y|n` (default y) on the queries that
  follow dependencies (deps, rdeps, why, orphans, broken, affected), held to the oracle, to
  depclean and to the fork's index with and without dynamic deps, on every scenario and the live
  system. The trees of the evaluated store replace the installed ones when the store is opened,
  so every query runs unchanged. depclean's `_minimize_children` is now emulated: the ebuild's
  `:=` atom beside the built one it appends selects the same package. The fork passes its own
  setting, so its `on` mode uses egraph under emerge's default too.
- 9d (done): `deps`/`rdeps --possible`: what the ebuilds' dependencies would add with USE flags
  toggled, each with the fewest toggles that add it (`use=a -minimal`), stored per installed
  package in the evaluated store. Only flags in the ebuild's IUSE that the profile neither
  masks (to turn on) nor forces (to turn off) count. Held to an oracle that tries every set of
  toggles on the scenarios and every single toggle on a live sample.
- 9e (done): `egraph updates [-N|-U]`: what `emerge -pu @installed` would replace (a newer
  visible version in the slot, or another one when the installed version has no visible
  ebuild), and with `--newuse` or `--changed-use` what it would rebuild for USE, with the flags.
  Held exactly to emerge on every scenario in all three modes; on the live system it lists
  everything emerge does, plus what dependents' bounds hold back (recorded in findings). The
  evaluated store's visibility and mask bits let `orphans` and `why` pass over masked installed
  packages as depclean does, in both `--dynamic-deps` modes.
- 9f: freshness and hooks. Incremental rebuilds from the categories whose metadata changed; a
  `postsync.d` entry installed beside the `post_emerge` one, and the post_emerge entry refreshing
  both stores.
- 9g: the TUI shows pending updates on package pages and in a list filter, and possible
  dependencies marked with their flags.

## 10. Output and UX

Taken ahead of 8 and 9 at the user's direction: output need not look like portage's.

- 10a (done): a human layout of every query on a terminal, tab-separated lines otherwise, with
  colour, kind shorthand and Nerd Font glyphs.
- 10b: a TUI to browse the dependency tree, `why`, `orphans`, `broken` and `check`. Step 1
  (done): `egraph tui` on Notcurses, an optional meson feature, showing the store and quitting.
  Step 2 (done): the package list with incremental search (`/`), and package pages listing what
  a package depends on and what needs it, with the kind matrix; Enter follows a link, Esc goes
  back. Step 3 (done): both lists unfold in place as trees, stopping at cycles. Step 4 (done):
  pages open with `why`'s chain, and the list marks roots and orphans and can show only
  orphans, with or without build-time deps. Step 5 (done): broken packages, marked and
  filtered in the list, and their unsatisfied dependencies on their pages. Step 6 (done):
  `check` from the list, the drift linked to package pages. Step 7 (done): `u` in the check
  view rebuilds the store as root, or previews the fresh build for anyone else. Step 8 (done):
  errors and notices in dialogs.
- `rdeps --possible` (dependencies behind disabled USE flags): done in 9d.

## 11. Build monitor

Watching an emerge that runs elsewhere, at the user's direction; egraph never starts or drives
one. Not scheduled ahead of 8 and 9.

- Source: portage's `FEATURES="observability"`, which publishes each running emerge as
  `/run/portage/emerge-<pid>.json` (schema 1, world-readable; the socket beside it is root's
  only), as `portageq jobs` reads it. A file whose pid is not its name's, or not alive, is
  stale. The remaining merge list is mtimedb's `resume.mergelist`. Both are JSON, which the C++
  side cannot read yet: nlohmann_json (header-only, value-typed, parses without exceptions) is
  the candidate.
- 11a (done): a build view in the TUI. Each running emerge with its progress (done of total, as a bar
  and a percentage), and its tasks as a hierarchy with glyphs: kind (build, binary, merge,
  waiting to merge), phase, a spinner while running, elapsed time, and the cgroup's CPU
  parallelism and peak memory when `FEATURES=cgroup` reports them. The screen needs a read with
  a timeout so the view refreshes about once a second.
- 11b (done): pressure. CPU, available memory and load from `/proc`, and PSI from `/proc/pressure`
  when the kernel has it on (`psi=1`; the dev box builds it disabled by default), as sparklines
  with steve's thresholds (load average, minimum available memory) marked.
- 11c (done): steve. Tokens in use of the total and its settings, through `stevie`'s getters; changed
  live through its setters when `/dev/steve` is writable (root, or the `jobserver` group), and
  read-only otherwise. Which process holds which tokens steve only prints to its log on
  SIGUSR1; showing that needs an interface upstream.
- 11d (done): the pending packages as a hierarchy under what they wait for, with the same
  glyphs and spinners, dropping out as they merge. Dependencies of versions not yet installed
  come from a builder pass over just the merge list (`egraph-build --pending`), not the
  evaluated layer.
