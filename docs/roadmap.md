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
| 9 | Evaluated and candidate layers | done |
| 10 | Output and UX | done |
| 11 | Build monitor | done |
| 12 | Updates of `@world` | done |
| 13 | A living app | done |
| 14 | Plans | done |
| 15 | A system package | done |
| 16 | Daily use in place of emerge | done |
| 17 | `egraphd`, the service | planned |
| 18 | Explaining and checking the configuration | planned |
| 19 | What-if | planned |
| 20 | Build knowledge | planned |
| 21 | The fork's speedups upstream | planned |
| 22 | Portage's tools in egraph's space | to map out |

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
- 9f (done): freshness and hooks. The evaluated store is rebuilt incrementally: a cp is
  evaluated again when its installed packages changed, when its category's metadata cache
  changed in any repository, or outside the main repository when its own directory or ebuilds
  did; the rest only have atoms naming a changed cp matched again, and anything global means a
  full build. `EGRAPH_STRICT` holds it to a full build. The `egraph refresh` hook is installed
  in `postsync.d` beside `post_emerge.d`; the post_emerge entry already refreshed both stores.
- 9g (done): the TUI reads both stores, with dynamic deps and masks as the queries do; it
  marks pending updates in the list, filters on them with `u`, and shows each on its package
  page, with the possible dependencies both ways marked with their toggles.

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
- 10c (done): glyphs default to ASCII outside a UTF-8 locale.
- 10d (done): the TUI's rebuild saves the check's fresh build instead of running a second one.
- 10e (done): a named option value outside its choices fails naming them, and the numbers
  behind the names are no longer accepted.

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

## 12. Updates of `@world`

`egraph updates` answering what `emerge -uDN @world` would, as far as that needs no resolver.
emerge is the check, not the specification: where egraph's answer is better (naming what holds
an update back, rather than skipping it), it stays, recorded in `upstream-notes.md`.

- 12a (done): atoms match candidate ebuilds, with the USE each would be built with now, as depgraph
  matches an ebuild against a dependency (`egraph match --candidates`), in shadow against
  depgraph's own matching on every scenario. Atoms record slot operators.
- 12b (done): bounds. A target that an installed dependent's atom rejects is held back, and the
  update falls back to the best visible version in the slot every such atom accepts. A slot
  operator does not hold: emerge rebuilds the dependent instead. `updates --held` lists what is
  held and by whom. Compared with `emerge -puD @installed` on every scenario; on the dev box it
  leaves 5 of the 12 differences with `-uDN @world`: 2 for 12c, 3 for a resolver.
- 12c (done): `updates --world`: only the packages `emerge -uD @world` reaches (those depclean
  keeps) are updated, and only their dependents hold. emerge's arguments are then the root
  sets' atoms rather than every installed package, which shows in two ways: a `||` keeps an
  installed alternative no root atom names rather than switch to another (`bounds`), and a kept
  slot-operator dependent moves to the best visible newer slot when nothing else pins the old
  one, as `--rebuild-if-new-slot`, updating or pulling in that slot's package (`repository`).
  Equal to `emerge -puD @world` (plain, -N, -U) on every scenario and on the dev box, where the
  23 merges of `-N` are emerge's, the two gentoo-sources rebuilds outside @world gone.
- 12d (done): remedies for a held update, as a choice rather than emerge's silent skip. Removing a
  holder is offered only when it is a leaf: nothing installed depends on it, and only world (or
  nothing) keeps it; then the update goes through, and what else it frees is listed. A holder
  something else needs is named, not offered. Keeping a holder while updating is only possible
  through `--nodeps`, which later `-uD` runs undo, so it is shown with that warning. egraph prints
  the commands (`--deselect`, `-C`, the update); it does not run emerge.
  - 12d1 (done): `updates --held` (`remedy.cpp`): each holder with its dependents and root
    atoms, a rebuild's ebuild standing for its installed package; the removal checked by
    planning again without the holders (under `--world`, in the scope depclean keeps then),
    with the held updates it frees. Every removal is emerge's on the system without the
    holders, on every scenario. On the dev box, removing openrgb-plugin-skin (world only) lets
    openrgb 1.0 through and frees the two plugins it held; the other six of the 9 held
    updates have holders something else needs.
  - 12d2 (done): the TUI weighs updates with the plan (`-N`, @installed): the list marks what
    it holds back, and a held package's page lists its holders as links with what keeps each,
    then the remedies' commands.
- What the targets' own dependencies pull in (new packages and slots, and updates that need a
  held one) and slot-operator rebuilds moved to step 14.

## 13. A living app

Taken ahead of 12c and 12d at the user's direction: egraph interactive whenever the user is,
and run-and-done otherwise (`docs/vision.md`).

- 13a (done): a `Session` owns what every command now loads per run (both stores, the dynamic-deps
  store, the graph, depclean's results, the pending updates), computed on first use; each
  command computes from a session and renders. Every one-shot command's output unchanged.
- 13b (done): a line-mode shell (`egraph shell`) reading commands from standard input, for builds without Notcurses and
  for tests.
- 13c (done): bare `egraph` on a terminal opens the TUI (`egraph tui` stays), with a `:` command line
  taking the CLI's commands and options through the same parser, and their results as views
  linked to package pages. A failed command is a message, not an exit. Off a terminal, bare
  `egraph` is the shell. The interface still keeps its own copy of the stores beside the
  session's until 13d.
- 13d (done): the app stays current: inputs checked on each tick, the incremental builder run as a
  child process watched without blocking, the new stores swapped in keeping the user's place.
  Check and rebuild stop freezing the screen the same way.
  - 13d1 (done): check and rebuild run the builder as a child process (`os::start`) the
    interface polls, with a spinner while it waits; leaving the view, or quitting, stops it.
  - 13d2 (done): the stores' inputs looked at every 2 s while idle, a refresh made in the
    background as a session opens stores (a current system store, else an incremental build),
    and the new stores shared with the session (`Session::adopt`) and swapped in keeping the
    list, pages and cursors, a page following its package across an upgrade. The interface no
    longer keeps its own copy of the stores.

## 14. Plans

What `emerge -uD` would merge, and in what order, rather than only what it would replace. Taken
ahead of 12c, 12d and 13d at the user's direction. The resolution is greedy, as far as emerge's
is on the systems we compare: a dependency nothing planned satisfies takes the best visible
version that matches it, and a `||` group takes its first alternative that can be satisfied.
Blockers stay out of scope until a comparison needs them.

- 14a (done): the targets' own dependencies in the evaluated store (format 4): per visible
  candidate, its dependency node lists reduced under the USE it would be built with, matches
  naming installed packages, held to depgraph's own reduction on every scenario; and
  candidates for every cp outside the installed set that those lists (or the installed
  packages' dynamic ones) reach where nothing installed satisfies them, followed to closure and
  kept incrementally. On the dev box: 5,023 candidates, 36 more cps (`dev-cpp/mm-common` among
  them), a full build 12% more instructions.
- 14b (done): `updates` as a plan (`plan.cpp`): new packages each target (or a kept package's
  dependencies) pulls in, with what pulls them; a target whose dependency nothing the plan can
  hold satisfies is held by it in turn, and an installed package whose update a target's atom
  rejects falls back, to a fixed point. The closure follows every alternative of a `||` an
  update could break. Equal to `emerge -puD @installed` on every scenario; on the dev box, the
  25 merges are `emerge -puDN @world`'s 23 plus two USE rebuilds outside @world (12c), with
  `dev-cpp/mm-common` pulled in by `glibmm`. 5,079 candidates, 54 more cps, a full build 81.3G
  instructions.
- 14c (done): slot-operator rebuilds: a kept dependent bound (`:S/SS=`, through DEPEND,
  RDEPEND or PDEPEND) to a sub-slot a merge replaces is rebuilt from a visible ebuild of its
  version, whose own dependencies join the plan (and pull in what they lack); with none, the
  binding holds the merge back to a version in its sub-slot. The line names the merge it is for.
  Equal to emerge's rebuilds on every scenario (`slotops`, `bounds`).
- 14d1 (done): the plan in merge order, `updates -t`: each merge numbered, with the places of
  the merges it waits for (DEPEND, BDEPEND, RDEPEND, IDEPEND), cycles broken at a run-time wait
  first, libc first as emerge's implicit dependency. Every wait emerge's order keeps is kept, and
  the waits listed are `pending.py`'s, on every scenario.
- 14d2 (done): the plan as a tree, `updates --tree`: each merge under its root set, down `why`'s
  chain for a replaced or rebuilt package and the pulling chain for a new one, merges with their
  place and waits. Held to `why` on every scenario.
- 14e (done): the tree as a view in the living app (`p` from the list), its installed packages
  opening their pages, kept on its package across a refresh.

## 15. A system package

egraph installed as any Gentoo package is: from an ebuild (`app-portage/egraph`) in an overlay,
built from a release tarball, tested against the portage users run rather than our fork.

- 15a (done): packaging basics: the licence file, a version, `meson dist`, and defaults a packager can
  build with (the installed portage, no checkout's test keys); the development builds name the
  fork explicitly.
- 15b (done): shell completions (bash, zsh, fish) generated from the CLI's own definition, so
  they cannot drift from it, and installed where each shell looks. Options and commands, fixed
  choices, directories and files by the option's type name, and installed cps from the vdb under
  `${ROOT}`; each script is loaded by its own shell in the tests.
- 15c (done): man pages, `egraph(1)` and `egraph-build(1)`, held to the CLI by tests that
  every command and option is documented, each command's under its own subsection, and linted
  with mandoc.
- 15d (done): portage hooks a package can install: `/etc/portage/bin/post_emerge` is the user's own
  file, so the dispatcher is installed beside the hooks for the user to link, and postsync.d
  (which portage runs itself) takes egraph's hook directly. Step 17 replaces both.
- 15e (done): the ebuild, `app-portage/egraph` in the Bonbon overlay (SuperFes/Bonbon), from
  the GitHub release tarball (github.com/SuperFes/egraph, v0.1.1): python-single-r1 over
  portage's Python versions, `tui` as a USE flag, the whole suite in `src_test` (the playground
  tests skip without portage's test keys). Its test run found portage's exported
  PORTAGE_CONFIGROOT reaching the unit tests, which 0.1.1 fixes.
- 15f (done): continuous integration on a stock stage3 (`.github/ci.sh`, run by GitHub Actions
  on every push and weekly): the whole suite against the portage release the stage3 ships, with
  that release's test keys from its tag, as a user, dependencies from the official binhost, so a
  portage change that breaks the builder shows up before users see it. Without the terminal
  interface, whose Notcurses is ~amd64 only; `screen.cpp` compiles against ::gentoo's 3.0.8 too.
  First run: gcc 15.3, portage 3.0.82.2, 1,538 passed.

## 16. Daily use in place of emerge

egraph for what emerge is used for every day: it decides what to merge and in what order, and
emerge builds and merges it. Decided with the user (2026-09-29): actions need egraph run as root
(as a user it plans and shows, and refuses to act); a plain update is `emerge -u @world`; no
binary packages (every merge builds from source).

Trust comes in two phases. First egraph plans and asks, then runs emerge on exactly the plan's
cpvs (`--oneshot`, the requested packages selected), so emerge's own checks and order still apply
and any difference from egraph's plan stops the run and is recorded; with the targets fixed,
emerge skips its deep walk over @world. Then egraph schedules and runs the merges itself
(16k), each stage held to emerge's own runs.

- 16a (done): the plan for plain `emerge -u @world`: `updates` without `-D` updates only
  emerge's arguments, drops an update anything rejects rather than fall back or rebuild a
  slot-operator dependent, replaces an installed package only where a merge needs a newer
  version, and lets kept packages' dependencies only reject. `-D` is the deep plan as before.
  Beside it, two gaps of the deep plan: a root atom's best version in an empty slot is pulled in,
  and a merge may need a newer version of a package outside the scope. Equal to `emerge -pu` and
  `-puD` (@installed and @world, plain, -N, -U) on every scenario, `shallow` new among them, and
  on the dev box, where `--world` merges 5 against `-D`'s 23.
- 16b (done): plans for any request, not only updates: atoms, `=cpv`, slots and sets as targets,
  `--oneshot` and `--noreplace`; a cp outside the store evaluated by the builder on demand and
  kept; a new package's USE shown.
  - 16b1 (done): `plan TARGET...` with `-u`, `-D`, `-n`, `-N`, `-U`: the targets resolved
    against the stores, emerge's greedy slots, reinstalls and `--noreplace`, backtracking for
    what an argument must merge, and `-uD` recursing only from the arguments. Equal to `emerge -p`,
    `-pu`, `-puD` and `-pn` for 1660 requests over every scenario but four `-uD` atoms in
    `slotops` (cascading slot-operator rebuilds outside the reach, which emerge drops), settled
    in 16c. `--oneshot` only matters to the actions, so it moves to 16f.
  - 16b2 (done): a cp outside the stores evaluated by the builder on demand: the evaluated
    store lists every repository cp (format 5), so names resolve as emerge resolves them (the one
    category outside virtual and acct-* beside the others), and `plan` has `egraph-build
    --evaluate` read what only the repositories know, with what it pulls in. Kept through every
    later build until `egraph rebuild`, or until installed, since configuration edits make full
    builds too often to drop it at. Every scenario's requests are now compared with every cp
    evaluated, with no difference from emerge.
  - 16b3 (done): a new package's USE shown as `emerge -v` shows it: a group per USE_EXPAND
    variable not hidden, enabled flags then disabled ones in emerge's alnum order, and those the
    profile forces or masks in parentheses (the evaluated store keeps both, format 6). In
    `updates` and `plan` lines, on its own line in human output, and in the living app's plan.
    Equal to emerge's own `_display_use` for every new package of every compared plan.
- 16c (done): `--verify` on `updates` and `plan`: the same request through the real
  `emerge --pretend --verbose` (without EMERGE_DEFAULT_OPTS), its merge list read back and
  compared: what is merged from which repository, the kind of merge, and a new package's USE,
  equal versions counting as one. Differences follow the plan (on standard error in the lines
  layout), with exit status 5.
  `test_verify.py` holds it to the emerge binary on every scenario. That caught
  `--usepkg=n` turning off `--with-bdeps`' default, a portage bug (`upstream-notes.md`). It also
  settled the four `slotops` atoms pinned in 16b1 as egraph's error: under `-uD`, emerge
  rebuilds outside the reach only for a merge that breaks a binding within it, and drops the
  merge otherwise. On the dev box, `updates`, alone and with `-D`, `--world` and `-N`, agrees
  with emerge, up to 47 merges.
- 16d (done): blockers, weak and strong, as a query and weighed by the plan (formerly step 17).
  `blockers [PACKAGE...]` lists the installed packages' blockers with what each blocks, equal to
  portage's matching on every scenario. The plan weighs them as emerge's `_validate_blockers`
  does once its graph is complete: an uninstall where nothing in the completed graph needs the
  blocked package (or the installed holder), a block otherwise, and between two merges; strong
  blockers only resolved by a replacement on the running root. `-u`'s greedy slots leave out a
  slot the best version blocks. Uninstalls and blocks follow the merges in `updates` and `plan`,
  a block makes the exit status 6, and `--verify` compares both. Two scenarios, `blockers` and
  `blocked`, give 60 requests emerge resolves with uninstalls and 106 it refuses, all equal.
- 16e (done): what makes emerge refuse: REQUIRED_USE and invalid metadata on candidates; a
  needed USE change explained as autounmask would, the `package.use` line offered and written
  only on yes.
  The candidates' REQUIRED_USE tokens and conditional dependency trees are stored as portage
  splits and parses them and evaluated by egraph, shadow-tested against portage, rather than
  precomputed for the current USE.
  - 16e1 (done): dependencies nothing satisfies, as emerge's backtracking weighs them: a pulled
    candidate whose dependency no visible version matches is masked and what pulled it chooses
    again; what emerge must merge with no version left refuses the plan (exit status 6, now for
    blocks too), the dependencies at the chains' ends listed. Ebuilds with invalid metadata are
    masked as depgraph masks them. Every scenario's requests that emerge refuses for them are now
    compared, not skipped; a `refused` scenario holds the cases, and `--verify` reads emerge's
    refusal.
  - 16e2 (done): REQUIRED_USE, checked wherever emerge selects a version (evaluated store format
    7 stores the tokens and the EAPI's empty-group rule), the constraints left unmet listed as
    emerge words them, the plan refused; `--verify` reads emerge's unmet requirements. A
    `required` scenario holds the cases, and the check is shadowed against portage's over
    generated strings and the live repositories'.
  - 16e3 (done): USE changes as autounmask proposes them.
    - 16e3a (done): evaluated store format 8 keeps each candidate's dependency tokens, and
      `use_reduce` is ported to reduce them under other USE, shadowed against the builder's
      reduction.
    - 16e3b (done): the planner's autounmask: a USE dependency no version meets as it is built
      meets it with its flags changed, the plan made again with its dependencies reduced under
      the new USE; the plan shows the changed USE and the `package.use` lines with their
      "required by" chains, and is refused. The builder reaches what the changes could pull in.
      A `usechange` scenario holds the cases, and each plan is held to what emerge merges once
      package.use makes the changes.
  - 16e4 (done): `--verify` over the new refusals (REQUIRED_USE since 16e2, the USE changes as
    sets of flags); on a terminal the `package.use` lines are offered and, on yes, written, the
    stores refreshed and the plan made again. An argument whose USE dependencies only a change
    meets is planned, as emerge's autounmask does.
- 16f (done): actions: `update`, `install` (`--oneshot`, or the targets selected), `remove` (through
  `emerge --depclean` with atoms), `select`, `deselect`: the plan shown, a yes asked for, emerge
  run on the plan, the stores refreshed at once. emerge resolves the request again itself, so
  each plan is verified against `emerge --pretend` first and nothing runs when they differ.
  - 16f1 (done): `update` (`emerge -u --oneshot`) and `install`: stopped before emerge when the
    plan is refused or empty, or the vdb cannot be written (run as root); `--yes` where there is
    no terminal to ask on; EMERGE_DEFAULT_OPTS ignored but for its execution options (`--jobs`,
    `--keep-going`, ...), read through `egraph-build --emerge-options`. `test_actions.py` merges
    for real on a playground through the real emerge, the stores refreshed after.
  - 16f2 (done): `remove`: `emerge --depclean` with atoms, planned as depclean weighs
    arguments (the world file's atoms left out, every installed package not matched kept) and
    verified against its pretend output; what stays is listed with what keeps it. The store
    records the world_sets set an atom of @selected comes through (format 5), which depclean
    keeps; remedies no longer offer to deselect such an atom. Equal to `_calc_depclean` for
    every installed cp on every scenario.
  - 16f3 (done): `select` (`emerge --select --noreplace`, the plan for what is not installed
    verified) and `deselect` (`emerge --deselect`, the atoms as emerge's pretend names them),
    with the packages depclean would then remove. Which atom emerge records is its own
    (`create_world_atom` reads the repositories' slots), so after every action the atoms that
    joined or left @selected are read back from the refreshed stores.
- 16g (done): after a merge: the elog summary, pending `._cfg` files (`dispatch-conf` a key away),
  preserved libraries planned as their consumers' rebuilds, unread news.
  - 16g1 (done): `notices`: the configuration files waiting in `._cfg` updates, as emerge finds them,
    and the unread news, with their titles, read through `egraph-build --notices`; shown after
    every action that ran emerge, `dispatch-conf` offered on a terminal.
  - 16g2 (done): preserved libraries in `notices`, with the packages using them; the rebuild of their
    consumers planned as `emerge --oneshot @preserved-rebuild`, verified against its pretend
    output, and offered after an action.
  - 16g3 (done): the elog summary after a run, read from the log `save_summary` appends to while emerge
    runs, emerge's own echo of it left out then.
- 16h (done): `sync`: `emaint sync -a`, then what the sync brought: the updates, as `updates`
  shows them with the same options, and the notices, the news the sync marked unread among
  them; both after a failed sync too. `test_actions.py` syncs a playground by rsync from a
  mirror through the real emaint.
- 16i: a repository index from the builder (every cp, its description, versions per slot,
  keywords and masks), incremental on sync; search across the repositories, and pages for
  packages not installed. Shell completion answered from it by a hidden `egraph complete`, which
  loads the stores without a freshness check: every cp in the repositories (the vdb alone offers
  too little to be useful), names without their category, versions after `=`, slots after `:`,
  and sets.
  - 16i1 (done): `egraph complete`, hidden, for the scripts: the repositories' cps from the
    evaluated store, versions and slots of what the stores hold, repositories after `::`; the
    `ATOM` and `REPOSITORY` arguments apart from installed `PACKAGE`s.
  - 16i2: the repository index (`installed.repository.egraph`, `egraph-build --repository`):
    per version its slot, KEYWORDS, LICENSE, PROPERTIES, RESTRICT, EAPI, description and
    homepage, with the configuration that decides visibility as portage parsed it
    (ACCEPT_KEYWORDS and the package.accept_keywords layers, ACCEPT_LICENSE with its groups
    expanded and package.license, package.mask and package.unmask stacked, ACCEPT_PROPERTIES,
    ACCEPT_RESTRICT). Decided with the user (2026-10-06): visibility is evaluated in C++, not
    precomputed by the builder, so that what-if questions need no builder run.
    - 16i2a (done): the index written by the builder, read by egraph (freshness, refresh, `export
      --repository`), held to portage's own metadata on every scenario and to the builder's JSON.
    - 16i2b (done): visibility and its reasons evaluated in C++ (`versions`), shadowed against
      portdb's match-visible and getmaskingstatus on every version of every scenario and the
      live system, which agree on all 38,544.
    - 16i2c (done): incremental refreshes: only the visibility configuration (and the USE of the
      versions keeping one) after a configuration change, only the changed categories and cps
      after a sync, held to a full build under EGRAPH_STRICT.
    - 16i2d (done): completion offers every version, slot and repository from it; `refresh`,
      which the portage hooks run after every emerge and sync, keeps it current.
  - 16i3 (done): `search`, by name, or with `-S` by description, as `emerge --search` (its
    regex, category and fuzzy matching, its choice of version, and the description its
    IndexedPortdb searches), held to emerge's own search on every scenario and the live
    system; and depgraph's validity per version in the index (format 3), which search's
    visibility and `versions` follow.
  - 16i4 (done): search in the living app (`s`, as `egraph search`, the index loaded or built
    in the background the first time), listings of packages not installed (their ebuild and
    every version with why it is masked), and versions on installed packages' pages.
- 16j (done): the actions in the living app: updates picked, installs from search, orphans
  removed, a confirmation, the run in the emerge view, a failure's log tail. Each is egraph's
  own command (`exec`, `remove`), previewed in the session and run beside the interface once
  confirmed; exec's run state records each failure with its log.
- 16k: egraph's own execution: `egraph exec` (`egraph-exec`, a link to `egraph`), which holds the
  order, the parallelism (with steve), priorities, failures and the display, and drives long-lived
  Python workers that call portage's public phase and merge functions; the ebuild machinery stays
  portage's. Decided with the user (2026-10-01), replacing one `emerge --oneshot --nodeps` per
  merge: `--nodeps` turns off blocker handling, and parallel emerges overwrite each other's resume
  list. emerge-equivalent ordering stays the default until real use shows the narrower waits safe
  (undeclared dependencies are why emerge's merge-wait exists). Each stage is compared with
  portage on playgrounds.
  - 16k0 (done): egraph's plan written as emerge's resume list (`updates` and `plan`
    `--resume-list`), and emerge's resume of it holding every merge, adding none, and resolving
    the same uninstalls, for every update mode and every plan request on every scenario
    (`test_resume.py`, through `_resume_depgraph`), and run for real by `emerge --resume`.
    Resuming, emerge orders the merges again from its own graph (it keeps the list's order only
    under `--nodeps`) and carries no uninstalls, so whether egraph's order is one emerge allows
    is 16k1's.
  - 16k1a (done): when each build may start and merge: each merge's waits with their kinds
    (build, install, run, post, emerge's implicit libc wait, and through installed packages
    that stay, which emerge's scheduler also waits on), in `-t`; held to the scheduler graph
    emerge builds from egraph's resume list (`test_waits.py`, every update mode and plan request
    on every scenario, and the `waits` scenario for every kind): the same direct edges with the
    same kinds but for `||` alternatives emerge did not choose, a superset of what it reaches
    through installed packages, and egraph's order keeping every wait emerge's order keeps.
  - 16k1b (done): the uninstalls' places: each waits for every merge whose blocker needs it
    gone (egraph kept only the first), in `-t`. emerge never uninstalls first: a merge cannot go
    while an uninstall is in its way, and the uninstall is only ever scheduled by reversing that
    edge, the two installed at once until it runs, straight after them as emerge prefers an
    uninstall (a strong blocker on the running root never gets that far, and is a block).
    Held to the scheduler graph's uninstall nodes in emerge's merge list (`test_waits.py`), the
    `blockers` scenario now with one uninstall two merges need, one blocking it and one it
    blocks.
  - 16k2 (done): a worker builds and merges one package; its vdb entry and image compared with
    `emerge -1`'s. `egraph-build --worker`, JSON requests in and events out, through portage's
    public `doebuild` and `merge` as emerge's `EbuildBuild` drives them, the configuration as
    emerge's Package sets it up; the same image, world file and vdb entries (but for when they
    were written) as `emerge -1` from the same state (`test_worker.py`): a new package with a
    protected file waiting as `._cfg`, one replacing an installed version (what only the old
    one installed removed, `REPLACING_VERSIONS`), EAPI 7, and two merges by one worker, the
    second's `has_version` seeing the first. A failed phase names its log and merges nothing.
    Not yet: `FEATURES=buildpkg`'s binary package (the builddir lock held across the phases
    came with 16k4a, `blockers` with 16k3b).
  - 16k3 (done): a whole plan run one build at a time, blockers and uninstalls included; the
    final system and world file compared with emerge's run.
    - 16k3a (done): `egraph` run as `egraph-<command>`, a link to it, runs that command, which
      then takes the global options among its own (the user's choice, 2026-10-01, over a
      separate binary).
    - 16k3b (done): the worker uninstalls (`{"uninstall": cpv}`, as emerge's
      `PackageUninstall`), hands a merge its blockers (dblink's collision checks, and the files
      the merge takes over leaving the blocked package's CONTENTS, so its uninstall keeps them),
      and records world atoms; held to `emerge -1` replacing a blocked package that shares a
      file with it, to `emerge -C` with and without a world atom, and to plain `emerge`'s world
      file (`test_worker.py`). Without its blockers, protect-owned refuses the merge. Not yet:
      the world_sets entries emerge drops when it uninstalls a package an argument matches
      (`setconfig.active`).
    - 16k3c (done): the plan as the worker's requests (`exec.cpp`; `updates` and `plan
      --requests`), each merge's blockers and world atom and each uninstall's world cleaning
      worked out in C++ as emerge's scheduler, `BlockerDB.findInstalledBlockers` and
      `create_world_atom` do; held to those, replayed in egraph's order on the depgraph emerge's
      resume builds (`test_requests.py`, every update mode and plan request on every scenario:
      blockers on 80 requests, world atoms on 650, 98 of them slot atoms). Where resuming emerge
      takes another ebuild of an equal version (`dev-libs/v-1.0` for `-1.00`), there is nothing
      to hold the request to. Not yet: an old-style virtual's providers, which
      `create_world_atom` weighs for a system virtual (profiles no longer have them).
    - 16k3d (done): `egraph exec` (and the `egraph-exec` link, installed): `install`'s action
      up to the confirmation, then the plan's requests run in turn through one worker
      (`os::Talk`), stopping at the first not done, a line per event. The same image, vdb
      entries and world file as `egraph install` with the real emerge from the same state
      (`test_exec_run.py`): a package with the dependency it pulls in, the same under
      `--oneshot`, an update replacing a version, and a blocker uninstalling a package whose
      file the merge takes over; a failed build names its phase and log and stops the run. The
      comparison found the worker's own `PYTHONPATH` saved in each package's environment, which
      `test_worker.py` missed by running emerge with the same one; the worker now drops it, and
      emerge runs there as a user's would. Not yet: dispatch-conf and the preserved-libs rebuild
      offered after the run (emerge's action offers them), the display (16k6).
  - 16k4 (done): parallel builds; a trace holds the rules emerge depends on (never two merges at once,
    no build before what it builds against is merged, portage's locks taken as portage takes
    them, tested against a concurrent emerge); timing against `emerge --jobs` is 16k8's. Workers
    pooled, since each spends about a second loading portage's configuration.
    The number of jobs is `exec -j`, else EMERGE_DEFAULT_OPTS' `--jobs`; steve only queues
    them, a token taken before each build starts and given back when it ends, as emerge's
    FEATURES=jobserver-token does, never deciding how many (the user's call, 2026-10-02).
    - 16k4a (done): the worker builds and merges in two requests, `{"build": ...}` then
      `{"merge": cpv}`, holding the build directory's lock from the build to its merge, as
      emerge's `EbuildBuildDir` takes it (through `portage.locks`), and around an uninstall;
      still the same image and vdb entries as `emerge -1` (`test_worker.py`), the lock seen
      held between the two from another process, and let go after a failed build.
    - 16k4b (done): the schedule (`schedule.cpp`), as emerge's scheduler with FEATURES=merge-wait
      (portage's default): a build starts once it reaches no merge yet to finish through what
      each merge waits for (deeply, through merges done and installed packages that stay),
      but those queued after it (`_dependent_on_scheduled_merges`), the first anyway when
      nothing else runs; builds run up to the jobs; a built package merges once no build runs,
      merges one at a time in the order their builds finished, no build starting until they
      are merged (`_merge_wait_scheduled`); an uninstall goes first once its merges are done;
      after a failure nothing new builds and what built still merges.
      The jobs from EMERGE_DEFAULT_OPTS (`jobs_of`). Tested on its own, with no workers.
      Narrower than emerge for now: merge-wait always on (FEATURES=-merge-wait is 16k7's
      narrower barrier, since done), no `--load-average`, and emerge's hold on unrelated builds
      while a merged @system package's run-time dependencies are unmerged is left out (16k7a
      adds it).
    - 16k4c (done): `exec` runs it over a pool of workers (polled together), writing a trace;
      the trace holds the rules, the final system is `install`'s, a concurrent emerge waits on
      portage's locks, and the timing beside `emerge --jobs` goes in findings.md.
      - 16k4c1 (done): a worker keeps several built packages, merged in any order, as merge-wait
        holds them; each merge and uninstall reads the mtimedb afresh, so workers beside each
        other keep what the others recorded there.
      - 16k4c2 (done): `os::wait_for` polls several workers' output at once, with a jobserver's
        pipe when a token is wanted; `os::Jobserver` takes and gives back its tokens as emerge
        does (read and write, without blocking).
      - 16k4c3 (done): `run_schedule` runs the schedule over a pool (a template, tested with a
        fake one): a build on an idle worker once a token is had, its merge on the worker that
        built it, an uninstall on any idle one, workers added when none is idle; a merge's
        request is written as it starts, its blockers as emerge's scheduler finds them then
        (`InstalledBlockers` after the steps done, since merges go in the order builds finish).
      - 16k4c4 (done): `exec -j N` (else EMERGE_DEFAULT_OPTS' jobs) runs the plan over
        `WorkerPool`, its workers `--background` with more than one job, as emerge's background
        mode sends build output only to the logs; `--trace` writes each step's start and end.
        Under FEATURES=jobserver-token, `--emerge-options` names MAKEFLAGS' jobserver and each
        build takes a token, the first held already when MAKEFLAGS is in the environment, as
        emerge does. `test_exec_run.py`: the trace holds the rules with two jobs from the
        configuration and with `-j3`, the system left is `install`'s (but for each COUNTER,
        which the merge order sets), the jobserver caps the builds and gets its token back,
        and an emerge of the package exec is building waits on the build directory's lock
        (its pkg_pretend takes it) until exec's merge. The worker now runs each phase once, as
        emerge does: `doebuild("unpack")` used to clean an unmarked WORKDIR and run pretend
        and setup again.
        A build starts beside others only with the free space in PORTAGE_TMPDIR emerge's
        `_can_add_job` wants (`--jobs-tmpdir-require-free-gb`, 18 GiB and 1 GiB per running
        build), which also reaches `install`'s emerge now; the playground's /tmp has less, so
        the tests set it to 0, as emerge then runs one job at a time.
        Narrower than emerge: pkg_pretend runs with each build rather than for every package
        before the first, and PROPERTIES=interactive does not bring builds back to the
        terminal.
  - 16k5 (done): a failure skipping only what depends on it, compared with what `emerge
    --keep-going` drops. `exec --keep-going` (else EMERGE_DEFAULT_OPTS', `keep_going_of`): once
    what ran after a failure has finished and merged, as emerge's scheduler drains, the steps
    left that emerge's `_resume_depgraph` drops are skipped (`keep_going.cpp`) and the run goes
    on (`Schedule::resume`, through `run_schedule`'s resume hook). The merges left, and what is
    installed now where no merge left replaces it by slot or cpv, satisfy each merge's
    dependencies (all kinds) and the run-time ones of each installed package reached through
    them; what has one unsatisfied goes, with what reaches it through the atom that reached it,
    unless something installed matches that atom; again until nothing more goes. An uninstall
    goes once none of its merges is done or left. When a pass drops no merge (an installed
    package a merge reaches lacks a run-time dependency), the run stops, as emerge refuses to
    resume. Each skip is reported with the atoms it is left without and traced as `skipped`.
    `test_exec_run.py`: with one and two jobs, the steps skipped are exactly what emerge drops
    (a dependent of a failed package, its dependent in turn, one needing the failed version)
    while a dependent an installed version satisfies stays, and the system left is `install`'s;
    and an installed package with a dependency nothing satisfies stops both.
    Narrower than emerge: an atom selects the best version among what it matches rather than
    what emerge's graph holds already; an uninstall whose merge is done still runs after the
    resume, where emerge weighs the blockers again (not compared).
    Found: emerge merges a PDEPEND's package before its parent when nothing else orders them,
    where egraph's plan puts it after (`plan --verify` compares sets, not order); 16k5a.
  - 16k5a (done): PDEPEND in the order as emerge's: a merge waits for its PDEPEND's package as
    for any dependency, and a cycle drops that wait first (emerge's `_ignore_runtime_post` comes
    before `_ignore_runtime`); the displays list it with the others. `test_waits.py` now holds
    PDEPEND's waits to emerge's order too (`slotops` broke it), and the plan's unit tests a
    PDEPEND alone and in a cycle with an RDEPEND.
  - 16k6 (done): the live view in the living app, through the status file the emerge view reads;
    egraph's own log, never `emerge.log` (16k6b3).
    - 16k6a (done): `emerge::snapshot_json` writes a snapshot as portage's `build_snapshot()`
      does (schema 1, keys sorted, `max` true for `--jobs` without a limit), read back the same
      by `parse_snapshot`, which now keeps each task's root, operation and start time.
    - 16k6b (done): `exec` publishes its snapshot under FEATURES=observability, as emerge's
      `ObservabilityMonitor` does (on each event, at most once a second, and every 2 s while
      nothing happens), compared with emerge's for the same build stopped in a phase.
      - 16k6b1 (done): the snapshot of a run (`Observer`, `observe.cpp`), from the schedule's
        stage of each step (`Schedule::stage`) and the phases and workers reported: a task from
        a build's start (an uninstall's from being let through) to its merge, a merge once
        built, waiting under merge-wait (its time frozen at the build's end) or let through
        (keeping its last phase); emerge's counts (merges done, failures, the merge queue with
        the merge running), the total the merges left after keep-going goes on.
      - 16k6b2 (done): `exec` publishes it (`StatusFile`) to `emerge-<pid>.json` under
        `${EPREFIX}/run/portage`, FEATURES read by `egraph-build --emerge-options`; the pool
        ticks every 2 s while no worker speaks (`os::wait_for` with a timeout) and tells each
        worker's pid, which the trace hook now carries. `test_exec_run.py`: with two jobs, one
        package held in its compile and one built and waiting to merge, exec's snapshot is
        emerge's but for times and pids (whether each is there compared), and neither leaves
        its file behind.
        Narrower than emerge: no cgroup resources, no socket streaming snapshots.
      - 16k6b3 (done): egraph writes none of emerge's files: the status file moves to its own
        `${EPREFIX}/run/egraph/exec-<pid>.json`, published on every run rather than under
        emerge's FEATURES=observability, and the emerge view reads it beside emerge's.
    - 16k6c (withdrawn): mtimedb's resume entry. egraph leaves emerge's files alone, and plans
      fast enough that resuming needs no saved graph (16k9). What stays: `emerge_myopts`, held
      to `parse_opts` through the shadow binary, for `--resume-list`, whose favorites now name
      the targets under `--oneshot` too, as emerge records them.
    - 16k6d (done): logging: to the journal when systemd runs (libsystemd, an optional feature;
      structured fields for the run, step, package and event), else
      `${EPREFIX}/var/log/egraph.log` as JSON lines; an option picks the journal, the file,
      both or neither. `exec` first: the run, each phase, each package built, merged or
      uninstalled with its times, failures with their logs, skips, and the end.
      - 16k6d1 (done): the sinks and the event format (`src/log.hpp`): `--log` and
        `--log-file`, the journal through `sd_journal_sendv` behind the `journal` feature, the
        file appended under portage's lock (`os::append_locked`); `meson test` logs to the
        playgrounds' files.
      - 16k6d2 (done): `exec` logs its runs (`RunEvents`), held to its `--trace` in the
        keep-going runs, and read back from the journal as from the file with `--log both`.
      - 16k6d3 (done): the commands that hand the system to emerge or emaint (`install`,
        `update`, `select`, `remove`, `deselect`, `sync`) log a run of two events (`HandOver`):
        its start with the command line, its end with the exit status and time; `egraph log`
        lists them beside `exec`'s runs.
  - 16k7 (done): portage updating itself mid-run, and a narrower merge-wait barrier offered as
    an option.
    - 16k7a (done): FEATURES=-merge-wait: a built package merges while builds run, merges
      still one at a time; only the merge-wait scope's packages (the fork's `--merge-wait-scope`: deep, the
      default, for @system and its run-time dependencies in the plan; system; toolchain, its
      fixed list; none) wait for no build to run and merge alone, and once one of them merges
      with run-time dependencies still to merge, no unrelated build starts until they have
      (`_system_merge_started`). FEATURES and the scope from `egraph-build --emerge-options`
      and `exec --merge-wait-scope`; the scope's packages from egraph's own plan
      (`merge_wait_steps`; deep follows installed @system members whether or not emerge's
      graph reached them, so it may hold more than emerge, never less), the schedule's rules
      unit-tested, and an `exec` run under FEATURES=-merge-wait held to them by its trace and
      to `install`'s system.
    - 16k7b (done): portage updating itself: when the plan merges `sys-apps/portage` into the
      running root of a system install, the workers run from a copy of the running portage
      taken before the run (`egraph-build --copy-portage`, then `--worker --portage-copy`), as
      emerge's `_prepare_self_update` copies its own (bin and lib, under
      `PORTAGE_TMPDIR/portage`), removed at the end. Tested under the installed portage (the
      sysportage build), where the PYTHONPATH saved with the package shows the copy ran.
      Left for when egraph has a package in the tree: the same copy of `egraph-build` itself
      when the plan merges egraph.
  - 16k8 (done): side-by-side runs against emerge on the dev box: the same world, `--verify`
    agreeing on `-uDN @world` (once 16q landed), `exec -u app-portage/egraph` merging egraph
    itself, and planning timed against `emerge -uDNp` (findings.md). Builds are not timed
    live: ccache gives whichever runs second warm compiles, so the scheduling comparison stays
    16k4c's playground one.
  - 16k9 (done): resuming and reading back runs. Estimates come from the build history (20).
    - 16k9a (done): `exec --resume`: the last run's arguments and what it merged, kept in
      `${EROOT}/var/lib/egraph/exec.json`, planned and verified again without the merges of
      what that run merged (a target would be merged again otherwise); `--jobs`, `--keep-going`
      and `--merge-wait-scope` given anew replace the run's.
    - 16k9b (done): `egraph log`, the logged runs read back from wherever `--log` writes (the
      journal through `journalctl -o json`, else the file): a line a run with how it ended, or
      one run's events; the journal's fields come back as text, which the summary reads as
      numbers.
- 16l (done): a gap in the deep plan's scope, which follows the installed versions' dependencies where
  `emerge -uD` follows those of the versions replacing them. An installed package only a merge's
  new dependencies reach (an orphan until then) keeps its version where emerge updates it, and
  one only the replaced version depended on (now an unchosen `||` alternative) is updated where
  emerge leaves it; `plan -uD` also goes into every `||` alternative, emerge only into the one
  it chooses. Found with the `waits` scenario before its installed packages depended on exactly
  what top-2 goes deep into (`updates -D`, `--world` and `plan -uD`). The reach now walks as
  emerge's `_create_graph` does, each package through the version it ends up with and each `||`
  deferred until the plain dependencies are in, then through one alternative; `--world` takes
  it as `plan @world` does. The `deep-scope` scenario holds each form.
- 16m (done): a new package in a new slot, beside installed ones, reported as emerge's `NS`:
  the kind `new-slot` in `updates` and `plan` with the installed versions in its other slots
  (`other_slots`), in the human layout, the tree and the living app; `--verify` compares it
  with emerge's `NS`. Held to vardb for every update mode and plan request on every scenario
  (`blocked`, `blockers`, `pulls` and `shallow` have one), and to the emerge binary.
- 16n (done): a set's atom naming no slot goes only to its best version, where egraph took every
  installed slot it matched, as emerge does for an atom named alone (`_greedy_slots`, for
  `AtomArg`s only). Found with `updates -DN --world --verify` on the dev box: an old
  gentoo-sources slot with a USE change was rebuilt where emerge leaves it. The `world-slots`
  scenario holds an old slot with a USE change, one with a newer version in it, and one a
  dependency reaches.
- 16o (done): replacing an old slot, per package and opt-in (the user's preference for wine-vanilla):
  when a set's atom naming no slot moves to a new slot, the run uninstalls the installed slots
  it leaves once the new one merges, if nothing else needs them. A list of atoms in an egraph
  configuration file, empty by default, so the default stays emerge's (an old kernel's sources
  are kept while it runs); never the running kernel's sources. `updates` and `plan` show the
  replacement, `exec` uninstalls natively, `update`/`install` depclean after emerge. The first
  of egraph's configurable defaults. Mapped out with the user (2026-10-05): the list is
  `${PORTAGE_CONFIGROOT}/etc/egraph/replace-slots`, one atom per line; only world atoms; the old
  slot goes in the same run, after the new one merges.
  - 16o1 (done): the list read (blank lines and `#` comments skipped, a bad atom an error
    naming its line; `--replace-slots FILE` or `EGRAPH_REPLACE_SLOTS` for another, which the
    tests point at `/dev/null`), and a pass at the end of the plan (`replace_slots`) adding an
    uninstall for each installed slot a world atom naming no slot leaves, after the merge of its
    new slot, unless a root atom or a dependency of any kind of what stays needs it (satisfied
    with it, not without). An uninstall's reason is then a blocker or none: `updates`, `plan`
    and the human layout show the merge replacing it; `--verify` leaves replacements out, as
    emerge only drops them at its depclean; `exec` leaves the world file alone. The living app
    shows no uninstalls yet; it reads the list with 16j.
  - 16o2 (done): the running kernel: on the running root, once a plan replaces slots,
    `egraph-build --kernel-sources` names the `/usr/src/linux-*` directories each of those
    packages owns (CONTENTS as `getcontents` reads it, parents included), and the plan is made
    again keeping (`Targets::kept_slots`) any owning the one the running kernel's
    `/lib/modules/<release>/build` (else `source`) link points to; every one when egraph-build
    fails. Asked on demand rather than stored: every package's CONTENTS is 124 MB on the dev box,
    0.47 s of each full build even from the page cache.
  - 16o3 (done): `exec` uninstalls a replaced slot as a step after its new slot's merge, the
    world file left alone; `update`/`install` hand `emerge --depclean =cpv...` the replaced
    versions once emerge succeeds, in the same run (its own logged hand-over), so emerge's
    depclean has the last word on what still needs them. Held to each other on a playground.
- 16p (done): emerge's "The following installed packages are masked" warning (requested 2026-10-05,
  for TeX Live's package.mask): each installed package the plan keeps whose installed metadata
  is masked, when emerge's graph reaches it or LICENSE masks it (`depgraph.py`'s
  `_masked_installed`), with its reasons and, for package.mask, the file and comment
  (`getmaskingreason(..., return_location=True)`), a comment shown once. The evaluated store
  records the reasons, file and comment for each masked installed package (format bump);
  `updates` and `plan` show them in the human layout, and `--verify` holds the list to
  emerge's.
  - 16p1 (done): the evaluated store's mask reasons for each masked installed package (where `masked`
    or `vdb_masked` is computed), under both dynamic-deps views, as emerge's
    `get_masking_status` words them for an installed `Package` (portage's `_getmaskingstatus`,
    then its invalid metadata and an undefined SLOT), and the `package.mask` file and comment
    when that is among them; evaluated format 9. Held to `get_masking_status` and
    `getmaskingreason` on a scenario masking installed packages each way.
  - 16p2 (done): which ones emerge lists (`_masked_installed`): kept by the plan (no merge
    replaces them; emerge's tracker drops a replaced one), not visible as installed
    (`_eval_visibility`: package.mask, LICENSE, invalid, an unsupported EAPI; the evaluated
    store's `hidden`, format 10), and in the completed graph the blocker pass builds or masked
    by LICENSE; a `masked` row in `updates` and `plan` with the reasons, file and comment lines,
    emerge's block in the human layout (also with nothing to update), held to depgraph's list
    on every scenario and update mode, and `--verify` comparing the cpvs with emerge's warning.
- 16q (done): a gap in the deep plan, found by `--verify` on the dev box (2026-10-06): `-uD` takes the
  best visible version of every dependency atom it walks, so an atom naming no slot (outside a
  slot operator, or the `||` choice installed versions satisfy, as virtual/wine's
  `app-emulation/wine-vanilla[wow64(-)]`) pulls a newer slot in beside the installed one (NS);
  egraph only does so for root atoms and slot-operator bindings. Plain `-u` keeps a satisfied
  dependency, as egraph does. Reproduced with a scenario of a slot-1 package with a slot 2
  available, under a plain atom and under `|| ( z[abi] z )`: test_verify fails in the `uDN`
  and `world` modes only. Planned as a dependency's pull, as `root_pulls_`, falling back to the
  installed version when rejected; the old slot stays (`replace-slots` remains world atoms').
  Landed as emerge's `_select_pkg_highest_available_imp` decides it: a graph node at least as
  high (an argument's installed package) keeps the dependency, as does a higher installed
  version whose ebuild is visible, while one without is passed over (y in the `dep-slots`
  scenario). With it, an argument whose new slot is backtracked falls back to the version an
  installed slot holds, which plain emerge reinstalls and `-u` keeps, where egraph refused.

- 16r (done): a USE dependency the installed version was built without (dev-db/redis[jemalloc]
  needing dev-libs/jemalloc[stats] once package.use enables it) was unsatisfied: a dependency
  only took the version an installed slot holds when newer or rebuilt for an autounmask change.
  emerge takes the best version that matches, a rebuild as configured (or an older version, its
  ebuild gone) when the installed one fails only the USE dependencies; a version range it fails
  still holds the merge back. The `use-rebuild` scenario holds it to emerge.
- 16s: the masked installed packages for a plan of atoms (found by `--verify` on the dev box,
  2026-10-06): emerge warns only of those in its graph, which it completes from the root sets
  only under `--complete-graph` (or a `--rebuild-if-*` option) or once a merge changes an
  installed package's version, slot or sub-slot, IUSE or enabled USE, or adds a new slot
  (`_complete_graph`'s complete_if_new_use, _ver and _slot, on by default); otherwise its graph
  is the arguments, the merges and what they pull in. egraph always completes it, so
  `plan dev-db/redis` (a rebuild, nothing changing) warns of biber and biblatex where emerge
  does not. `updates` agrees, `@installed` making every package an argument; nothing compares
  the masked list for `plan` requests yet.

## 17. `egraphd`, the service

The stores kept current by a service rather than by hooks, and what follows from always being
there when something changes. Decided with the user (2026-10-06): no query socket, since loading
and checking the stores costs 10–30 ms of a query (findings.md, 16k8) and a user already reads a
current system store; the store files stay the shared interface. Read-only: nothing it does
changes the system (the acting jobs, distfile prefetch and `egencache`, are vision items).

- 17a (done): `egraph watch`, which the `egraphd` services run (a command, not a link, as
  multi-call names are `egraph-<command>`): the directories holding the stores' recorded inputs
  watched with inotify (a directory input itself, the parent of any other: 6,219 on the dev
  box, from 14,164 inputs), so that what it watches is exactly what freshness checks, never
  the repositories' whole trees. Changes settle (3 s quiet, or a minute after the first) before
  the refresh `refresh` runs; a refresh that fails is logged and retried after a minute,
  doubling to an hour, or on the next change; one that leaves the stores stale (a change during
  it) counts as a change. SIGTERM, SIGINT and SIGHUP end it with status 0, through a pipe the
  handler writes (no signal mask, which the builder would inherit). The stores are let go
  between refreshes: 13 MB idle. The loop is a template over its watcher and clock, tested with
  fakes; the watcher (`os::Watcher`) on a real directory; the whole on a playground. The shell
  and the interface refuse it. The hooks stay, for systems without the service.
- 17b (done): the service: its own `egraph` user, in the `portage` group (the preserved-libs
  registry the notices read is root's and portage's), only `/var/cache/egraph` writable; a
  systemd unit and an OpenRC script (`services/`, installed by meson unless `-Dservices=false`;
  the unit's directory from systemd.pc, or `-Dsystemd_unit_dir`). The unit: `CacheDirectory=`,
  `ProtectSystem=strict`, `ProtectHome=read-only` (overlays may live in /home), no network,
  `@system-service` syscalls, nice 10 and idle I/O; `systemd-analyze security` 2.5. The OpenRC
  script: supervise-daemon, `checkpath` on the cache, logging through `logger`. Smoke-tested
  under the unit's sandbox as a user service (`PrivateUsers=yes`): a rebuild, and watch
  refreshing all three stores and stopping with 0. The acct-user/acct-group ebuilds (dynamic
  IDs, as an overlay sets them) are in the Bonbon overlay; the egraph ebuild picks them up with
  the next release.
- 17c: history, decided with the user (2026-10-06): recency is what matters (anything past a
  few months is broken by USE changes since), and unexecuted plans are not history. In
  `${EPREFIX}/var/lib/egraph` (state, not cache; the services' `StateDirectory=`), flat, as root
  and the service user both write there:
  - 17c1 (done): generations: when a refresh of the system store changes the installed packages or the
    root sets (`drift`, and the roots compared by value), the store it replaced is kept whole as
    `installed-<ended>.egraph`, named by the replacing store's build time: when the system left
    that state (its own build time is only the last refresh that found it unchanged), so the
    generation for a time is the oldest that ended after it (installed store only, 1.1 MB on the dev box: packages,
    USE, deps and the why of the time; the evaluated store and the repository index are today's
    view, not history). Thinned by age after each: all from the last day, then the newest of
    each day, none past `history_days` (`${PORTAGE_CONFIGROOT}/etc/egraph/egraph.conf`, the
    settings file 17d–17f share; 90 by default, 0 keeps none). Keeping history never fails a
    refresh: a warning, and none kept.
  - 17c2 (done): the event log, `history.log`, kept whole: a JSON line per merged, upgraded,
    downgraded (another version replaced in its slot), rebuilt (a new COUNTER, or other flags) and uninstalled
    package, oldest first. Merges are timed by their vdb entry's COUNTER file, which the merge
    writes (BUILD_TIME is when a binary package was built), uninstalls by the refresh that found
    them. The store records each package's COUNTER and merge time for it (format 6, the JSON
    export's format 4). Appended by writing it anew and renaming it over the old one under a
    lock on the directory, so root and the service user both can, whoever made the file.
  - 17c3 (done): `egraph diff [when]`: upgraded, downgraded, rebuilt (a new COUNTER, or other
    flags), new and uninstalled packages with the flags turned on and off, then the atoms each
    root set gained and lost; against the newest generation by default (what the last change
    did), the system as it was at an age (`12h`, `3d`, `2w`) or a date's local midnight, or a
    generation by name. Before the history's start (the oldest generation's newest merge), it
    says since when. Lines like `updates`', the human layout `updates`' columns, and `--json`.
    A generation from another store format cannot be read, and says so.
  - 17c4 (done): `egraph history [when] [package...]`: the event log, oldest first, in diff's
    columns after each event's time, from an age or date (as diff takes them); packages are
    matched by cp and version against each event's versions, before and after (the log holds no
    slots, repositories or USE, so an atom with them is refused). For each package the log saw
    arrive, its `why` chain in the first generation that ended after its merge and holds it (the
    system just after), else as kept now. As lines, the log's JSON objects as it holds them.
- 17d: the precomputed plan: after each refresh, `-uDN @world` planned once and a small status
  file written (updates, rebuilds, held, security, the last sync's age), which the living app
  opens on and a status bar reads without running anything. Decided with the user
  (2026-10-06): when is a setting, `plan = refresh | sync | never`, every refresh by default
  (1.3 s and 36 MB for the dev box's 152 updates).
  - 17d1 (done): `watch` writes `status.json` beside the store it refreshed: the output of
    `updates --world -D -N --held` as lines, the counts (upgrades, downgrades, rebuilds, new,
    held, uninstalls, masked, refused), each repository's snapshot time (`metadata/timestamp.chk`,
    as `emerge --info` shows it; none for a plain git checkout) and the build times of the three
    stores it was planned from, each with its path (a user's installed store may read the
    system's repository index), so a reader can tell it from a current one. Planned again when
    those differ from the file's (`sync`: only the repository index's, rebuilt by a sync or a
    configuration edit, not by a merge). Skipped where the user cannot write; never fails a
    refresh. Security waits for 17e's GLSA matching.
  - 17d2 (done): `egraph status`, reading it without planning: the non-zero counts, each
    repository's sync age and the plan's on a terminal, `key\tvalue` lines piped, `--json`;
    current while each recorded store still records its build time, read from the header and
    meta section alone (1 ms in all). Without `--store`, the system's file or the user's,
    whichever is current, else the newer. `--update` plans and writes it now, for systems
    without the service.
  - 17d3 (done): the interface, decided with the user (2026-10-07): the status file's @world
    plan counted in the list's corner by glyph (with the repository synced longest ago, and
    whether the stores changed since), and the list in pages, one per set planned:
    `@installed` (as before), `@world` and `@system`, turned with left and right (enter opens a
    package). The two sets plan as `exec -uDN` would, the set its arguments (an empty set
    plans nothing), each the first time it shows, in the background (`std::async`, the stores
    shared with the job; a plan made for stores since replaced is dropped), a spinner on its
    tab meanwhile; the updates filter, the plan view and `U` follow the page. Reading the
    stored lines instead was dropped: the views need the plan itself, and the corner needs only
    the counts.
- 17d4 (outlines, not scheduled; the user's, 2026-10-07):
  - utility pages for sets portage resolves on demand, `@preserved-rebuild` (opened by itself
    at the end of an emerge that leaves preserved libraries) and `@smart-live-rebuild`;
  - the other package sets, as a thing of their own: repositories ship any number of them;
  - a package's page showing every piece of metadata the stores hold, not only the ebuild's.
  - USE flags on a package's page, toggled in place: REQUIRED_USE (`^^`, `??`, `||`, conditionals)
    greys out or flips what a toggle excludes, and the plan is redone with the change, its new
    rebuilds and pulls lit up; the evaluated store already holds REQUIRED_USE and the dependency
    trees, so it needs no portage call. A trial edit at first; persisting it is open: perhaps a
    USE database egraph manages (system, package and wildcard levels) as the one place USE
    lives, importing the existing `package.use` files and again whenever they change, and
    rendered to one generated file portage reads; retiring the imported files is the user's
    choice, never automatic, but egraph warns when a file no longer matches what the database
    holds (a flag changed or dropped there), naming the file and line to update or remove.
- 17e: notifications, moved here from step 20: GLSAs matched against the store, a stale sync,
  broken soname dependencies after a merge, unread news, masked installed packages; in the
  status file and the log, and on the desktop through a user-side `egraph notify` (the service
  cannot reach a user's session bus). Checked as their inputs change: GLSAs, news and masks
  when the repository index is rebuilt, sonames, preserved libraries and `._cfg` files after a
  merge, a stale sync on every refresh (`stale_after`, a week by default). Every notice can be
  dismissed (until it changes), put off (an hour, a day or a week) or worked on now (a GLSA's
  upgrade planned, news read, a sync, `@preserved-rebuild`, the masked package's page,
  dispatch-conf).
  - 17e1 (done): GLSAs in the repository index, as portage's `glsa` module parses them,
    matched against the installed store in C++ (shadowed against `Glsa.isVulnerable`), in
    `notices`.
  - 17e2 (done): `notices` gains repositories synced longer ago than `stale_days` (7 by
    default), masked installed packages, and required sonames nothing installed provides.
  - 17e3 (done): every kind as one notice with a stable key and a fingerprint, in
    `notices.json` beside the status file, written as watch refreshes, new ones logged;
    `status` counts them.
  - 17e4 (done): dismissed and put-off notices, per user under `XDG_STATE_HOME`
    (`notices --dismiss`, `--later`, `--for`, `--all`).
  - 17e5: the interface.
    - 17e5a (done): a contextual hint bar (the keys for what is selected, `?` for all).
    - 17e5b (done): a notices page beside the sets, its tab counting them, the selected one's
      detail below; `x` dismisses, `z` puts off with a small chooser.
    - 17e5c: working on a notice from its page with enter.
      - 17e5c1 (done): a GLSA's update, a rebuild for missing libraries, `@preserved-rebuild`
        and a sync, as actions; a masked package's page.
      - 17e5c2: the news item shown, and marked read in portage's news files once closed, as
        `eselect news read` does (only set aside where they cannot be written).
      - 17e5c3: dispatch-conf for configuration updates, the interface stepping aside for it.
  - 17e6: `egraph notify`: one summary notification (org.freedesktop.Notifications over the
    session bus) with Dismiss, Later (a day) and Open, which starts a terminal on the notice;
    started by an XDG autostart entry.
- 17f: what a configuration edit did: on a change under `/etc/portage`, the plan before and
  after compared ("+4 rebuilds for USE=foo on media-libs/bar, 1 new, the plan now refuses: ..."),
  in the status file and as a notification. Linting proper stays step 18.

## 18. Explaining and checking the configuration

- Where a flag's state comes from: the profile stack, `make.conf`, `package.use`, with file and
  line.
- Configuration that does nothing: entries for packages not installed or flags outside IUSE,
  keywords already stable, masks matching nothing, entries that contradict each other.

## 19. What-if

Flags and `package.env` toggled in the app, per package or globally, the plan shown changing.
The service keeps a warm portage configuration in Python, so a re-evaluation costs a fraction of
an emerge run (`docs/vision.md`).

## 20. Build knowledge

- Build history from the emerge monitor's cgroup readings: time and peak memory per package,
  which gives every plan an estimate ("34 rebuilds, about 2 h, 6 GB peak").
- A build history store of egraph's own (todo, to map out; recorded 2026-10-05), written by
  `exec` as each step ends, never dependent on emerge's files:
  - What each build records: package, version, repository, USE, the build's environment
    (CFLAGS, MAKEOPTS, `package.env`, the toolchain's versions), wall and CPU time per phase,
    peak memory, the build directory's peak size, installed size, distfile sizes and fetch
    time, how many builds ran beside it and the load average and PSI over it, the jobs and
    steve's tokens it had, the outcome and the failing phase's log.
  - Estimates: a package's time from its own builds, scaled for USE, version and the
    parallelism it gets; for one never built here, from similar ones (build system, eclasses,
    distfile size). A plan's wall time from the schedule itself, run ahead with those times
    and the jobs (the critical path, not the sum): `updates`, `plan`, `exec` and a whole
    `@world` update say how long they will take, and the build view counts down by time, not
    packages.
  - Queries and views: a package's last builds as a table and a sparkline ("gcc, the last 12
    times"), the slowest packages, total build time by week or month, what rebuilds most often
    and why (subslot rebuilds, USE churn), packages that fail now and then, and a build that
    got slower (a toolchain or CFLAGS change, LTO), each a page in the living app.
  - Advice from it: a build whose directory will not fit a tmpfs `PORTAGE_TMPDIR` (offer a
    `notmpfs` env), one whose peak memory at these jobs nears what the system has (fewer
    jobs, or steve's memory floor), FEATURES=test's cost per package, a binary package offered
    when a build is long and a binhost has it, a finish time for a run started now.
  - Scheduling from it: among the builds ready at once, the longest first or the critical
    path first, beside emerge's order; a divergence from emerge, so behind an option and
    recorded in `docs/upstream-notes.md`.
  - Importing earlier build times from `emerge.log` (read only, marked as imported, coarser),
    never required.
  - Export as JSON or CSV. The storage format (SQLite, or an append-only file of our own with
    an index) is the first thing to map out; C APIs stay behind `os.cpp`-style wrappers.
- GLSAs matched against the store at once, and a filter in the list.
- Space: what removing a package frees with the orphans it leaves (the vdb's SIZE).
- Notifications: moved to step 17e.

## 21. The fork's speedups upstream

The portage fork's resolver speedups (`docs/upstream-notes.md`) proposed to portage one at a
time, lowest risk first. The dev box is the evidence base: 2,324 packages, 8 repositories, envs
per package, heavy USE and keyword rules. Breadth comes from shadow mode, which anyone can run
without changing what emerge does.

- Rebase the fork onto current master; measure each commit alone on the stable portage
  (`perf stat`, interleaved), output identical before any speedup counts.
- Skipping slot-operator probes without a candidate first: on by default already, about half of
  `-uDN`'s time.
- Then the query memo and `regenerate()` replay, opt-in first; then neighborhood completion over
  the in-process installed index, with shadow-mode results from other systems.
- Facts persisted across runs and the external index stay out until the in-process ones land.

## 22. Portage's tools in egraph's space

`ebuild` (as `egraph-ebuild`, named options, manifests, bumps), the rest of `sys-apps/portage`'s
programs, syncing with its verification (signed Manifests and commits, keys, gpkg
signatures, the privilege drops and sandboxes), the overlapping gentoolkit and portage-utils
tools, and the library underneath, so
that a system runs without portage installed; then egraph moves into `sys-apps`. Mapped out in
`docs/vision.md` ("Portage's tools in egraph's space"); not scheduled until the user decides.
