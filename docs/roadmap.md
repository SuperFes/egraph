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
| 16 | Daily use in place of emerge | planned |
| 17 | `egraphd`, the service | planned |
| 18 | Explaining and checking the configuration | planned |
| 19 | What-if | planned |
| 20 | Build knowledge | planned |
| 21 | The fork's speedups upstream | planned |

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
emerge skips its deep walk over @world. Once that has shown no differences through weeks of real
use, egraph schedules the merges itself.

- 16a (done): the plan for plain `emerge -u @world`: `updates` without `-D` updates only
  emerge's arguments, drops an update anything rejects rather than fall back or rebuild a
  slot-operator dependent, replaces an installed package only where a merge needs a newer
  version, and lets kept packages' dependencies only reject. `-D` is the deep plan as before.
  Beside it, two gaps of the deep plan: a root atom's best version in an empty slot is pulled in,
  and a merge may need a newer version of a package outside the scope. Equal to `emerge -pu` and
  `-puD` (@installed and @world, plain, -N, -U) on every scenario, `shallow` new among them, and
  on the dev box, where `--world` merges 5 against `-D`'s 23.
- 16b: plans for any request, not only updates: atoms, `=cpv`, slots and sets as targets,
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
- 16f: actions: `update`, `install` (`--oneshot`, or the targets selected), `remove` (through
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
  - 16f3: `select` (`emerge --select --noreplace`) and `deselect` (`emerge --deselect`), with
    what a deselect leaves for depclean.
- 16g: after a merge: the elog summary, pending `._cfg` files (`dispatch-conf` a key away),
  preserved libraries planned as their consumers' rebuilds, unread news.
- 16h: `sync`: `emaint sync -a`, then what the sync brought: new updates, news.
- 16i: a repository index from the builder (every cp, its description, versions per slot,
  keywords and masks), incremental on sync; search across the repositories, and pages for
  packages not installed. Shell completion answered from it by a hidden `egraph complete`, which
  loads the stores without a freshness check: every cp in the repositories (the vdb alone offers
  too little to be useful), names without their category, versions after `=`, slots after `:`,
  and sets.
- 16j: the actions in the living app: updates picked, installs from search, orphans removed,
  a confirmation, the run in the emerge view, a failure's log tail.
- 16k: egraph's own order, once 16c has shown no differences: each merge an
  `emerge --oneshot --nodeps`, run in parallel as the plan's waits allow (with steve), a failure
  skipping only what depends on it.

## 17. `egraphd`, the service

The stores kept current by a service rather than by hooks, and served from memory.

- Refresh on change: inotify on the vdb, the repositories and `/etc/portage`, debounced, an
  incremental build at low priority (nice, ionice, its own cgroup). It replaces the hooks and
  notices hand edits to `package.use` too.
- Unprivileged: its own user, only `/var/cache/egraph` writable (`ProtectSystem=strict`); a
  systemd unit and an OpenRC script.
- Queries over a Unix socket, the stores loaded once: the CLI and TUI are clients when the
  service runs, and read the store themselves when it does not. Users who cannot write the
  system store get current answers without building their own.
- History: earlier generations of the stores kept, for `diff` against a point in time and "when
  did this get pulled in, and by what".

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
- GLSAs matched against the store at once, and a filter in the list.
- Space: what removing a package frees with the orphans it leaves (the vdb's SIZE).
- Notifications: security fixes pending, a stale sync, broken soname dependencies after a merge.

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
