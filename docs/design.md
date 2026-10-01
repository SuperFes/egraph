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
| Evaluated | installed packages' dependencies as emerge reads them by default, and what their ebuilds would add with flags toggled; how `emerge -u` and depclean weigh each (visibility, masks, the version it would move to, the flags `--newuse` rebuilds it for); the visible versions of installed cps, and of the cps their dependencies would pull in, with effective USE and their own dependencies, and why installed ones are masked | repo metadata, `/etc/portage`, profile, the installed store | stored (9b-9e, 14a) |

The evaluated layer is its own file beside the installed store (`installed.evaluated.egraph`),
written by the same builder run and keyed to that installed store: it names packages by the
installed store's ids and records its build start, so rebuilding either makes it stale.
Dynamic dependencies follow emerge's FakeVartree rule (`builder/egraph_build/dynamic.py`): the
ebuild's dependencies with the built `:=` atoms appended when the same version is still in the
package's repository, else the vdb's with the repositories' package moves applied.

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
- `match --candidates` matches the candidates' ebuilds instead (the installed cps' and those they
  would pull in), as depgraph matches an ebuild against a dependency: its `Package` with the USE
  it would be built with now, and the profile's `IUSE_EFFECTIVE` for implicit flags (candidates
  do not record their EAPI, so an ebuild of EAPI 0 to 4 is matched as if it had it). It runs in shadow against `match_from_list` on such
  `Package`s over the same corpus, every scenario's atoms and every live atom. Atoms record slot
  operators (`:=`, `:slot/sub=`), which update holds treat as rebuilds rather than bounds.

## Sessions

Every command answers from a `Session` (`src/session.hpp`), which loads each piece on first use
and keeps it: the installed store alone (which needs no evaluated store), both stores, the
dependency store and graph per `--dynamic-deps`, and depclean's result per `--with-bdeps` and
`--dynamic-deps`. A one-shot command builds one and exits; the interactive app (roadmap 13)
keeps one for as long as it runs. Freshness is checked, and a stale store refreshed or warned
about, when a piece is first loaded. Depclean without dynamic deps reads the installed store
loaded with the evaluated one, so its packages line up with the evaluated store's masks.

Without a command, egraph is interactive: the TUI when standard input and output are both a
terminal, the shell otherwise (`printf 'updates\n' | egraph`). In the TUI, `:` opens a prompt
that takes the same command lines as the shell and answers them from the session the interface
was opened with, which shares its stores with the interface and takes each refresh it makes; the output is shown in the lines layout as columns, lined up across each run of
rows with the same number of fields, and a row whose field names an installed package opens its
page. Errors and warnings go to a dialog, and `:quit` (or `:q`) ends the interface.

`egraph shell` reads commands one per line from standard input and answers them all from one
session, prompting when standard input is a terminal. A line takes the command-line syntax,
global options included, except those that choose the stores (`--root`, `--store` and the
like), which belong to the session. A failing line is reported and the next one read; the
status is the last command's. `test_queries` holds every scenario's shell answers to the
one-shot commands'.

## Query output

Two layouts of the same records. On a terminal, queries are laid out for people:

- Grouped by the package asked about, aligned, with counts. `deps`/`rdeps` show each dependency
  once with the kinds it appears under as a letter matrix (`R··D·`: R runtime, I install, P post,
  D build, B build host), `why` is a chain from its root set, `broken` uses the same letters, and
  a legend follows any layout that uses them.
- Coloured part by part (category, name, version; operator, slot, repository, USE flags), in
  truecolor when `COLORTERM` says so and xterm-256 otherwise. `--color auto|always|never`, and
  `NO_COLOR` is honoured.
- Decorated with Nerd Font icons by default, ASCII when the locale is not UTF-8 (or names one
  that is not installed); `--glyphs unicode` or `ascii` (or `EGRAPH_GLYPHS`) for fonts without
  them.

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

## Shell completion

`egraph-completions` (built, not installed) prints each shell's script from the CLI11 definition
`configure()` builds, and the build installs them (`egraph.bash`, `_egraph`, `egraph.fish`).
What an option's value is comes from its type name: `DIR`, `FILE`, `COMMAND` and `PACKAGE`, which
`--help` shows too, and fixed choices from `one_of`'s `{a,b}` description. Packages complete as
the cps in the vdb under `${ROOT}`, read by the shell itself (a glob and a regular expression that
strips the version), so a key press never loads a store or starts a refresh.

## Terminal interface

`egraph tui` (roadmap 10b) is built when Notcurses is found (`-Dtui=auto`, the default). The
Notcurses C API is confined to `src/screen.cpp` behind the value-typed `Screen` (keys, pens,
cells), as `src/os.cpp` confines the OS; the app in `src/tui.hpp` is a template over the screen,
so it is tested with a fake one and needs no terminal. It shares the human layout's palette and
glyphs (`--glyphs`). `App` holds the state and what keys do to it; drawing only reads it.

- The package list filters on every key after `/` (case-insensitive, anywhere in the cpv) and
  shows how many packages each depends on and is needed by. Each row is marked with the root set
  that keeps it directly, or as an orphan, and whether it is broken. `o` shows only orphans, as
  `egraph orphans` does, and `!` only broken packages, as `egraph broken` does. `b` leaves out
  build-time dependencies (DEPEND, BDEPEND) for both, as `--with-bdeps n` does for orphans.
  depclean runs once at start and again on `b`.
- It opens both stores and reads dependencies as the queries do (`--dynamic-deps`, y by
  default), with depclean passing over masked packages as `orphans` does. Rows with a pending
  update show the version it moves to, or that it is rebuilt, and those it holds back say so,
  and the list opens on only those (`u` shows every package), as `egraph updates -N --held`
  weighs them (the plan, roadmap 12d2).
  A page says what the update is (the target and repository, the flags a USE rebuild is for, or
  the merge a slot-operator rebuild is for); a held one's page lists its holders as links, with
  the atoms that hold it and what keeps each, then the remedies' commands as `--held` prints
  them. After what the package depends on and is needed by, what
  its ebuild would depend on with flags toggled, and whose ebuilds would depend on it, each
  marked with the toggles (`deps` and `rdeps --possible`); what nothing installed satisfies is
  listed but cannot be opened.
- `p` shows the plan as `updates --tree` draws it: each merge under its root set, down the chain
  that keeps what it replaces (or what pulls a new package in), with its place in the merge order
  and the places it waits for. Installed packages open their pages; a new one says what pulls it
  in.
- The stores stay current while the interface is open. Every two seconds without a key
  (`stale_interval`), `run()` looks at their inputs as a query's freshness check does (a few
  milliseconds of `lstat`); once they changed, it opens current stores as a session would, in
  the background: a current system store as it is, else the store at `--store` or the user's
  after an incremental build. The title shows a spinner meanwhile, then each view is kept where
  it was: the list's query, filter and selected package, and every open page with its selected
  row, the plan view on its package, a package found again by cpv or else by name and slot (so a
  page follows an upgrade).
  Unfolded trees fold again. A failed refresh stays in the title and is tried again after a
  minute (`retry_interval`). None starts while the check view is open, whose build compares with
  the stores shown, and `--no-refresh` turns it off.
- Builds run as child processes (`os::start`) that `run()` polls as jobs (`src/job.hpp`) while
  the screen keeps answering keys; dropping a job stops its build (SIGTERM). The app itself never
  spawns anything.
- `c` runs `egraph check` from the list: a waiting view with a spinner is drawn while the fresh
  build runs, then lists the drift between the installed stores, each package's page a key away;
  `esc` stops the build. The builder's output goes to a log beside the scratch store rather than
  the terminal the interface owns; if the build fails, a dialog shows its last lines.
- `u` in the check view acts on drift. As root it saves the check's own build, kept in its
  scratch files until then: while nothing it was built from has changed, copies are renamed over
  the stores as the builder writes them (installed first, mode 0644, synced); otherwise it runs
  `egraph rebuild`. Either way it shows the result, and the session answers from it. Anyone else
  gets a preview: the check's own fresh build replaces the store in memory only, and the title
  bar says it is not saved. The user's cache store is left alone either way; it is refreshed on
  the next query.
- `e` watches the running emerges, from the snapshots portage publishes with
  `FEATURES="observability"` (`src/emerge.hpp`): each emerge's jobs and progress as a bar, and
  its tasks under it with a spinner, kind, phase, elapsed time and, with `FEATURES=cgroup`, CPU
  parallelism and peak memory. The view reads them again every second (the screen's read takes a
  timeout, and a tick with no key is a key of its own), and not while a page covers it. A task
  opens the page of its package's installed version. egraph only watches; it never starts an
  emerge. Below the emerges, where the terminal has room, a panel graphs the system over the
  last four minutes (`src/pressure.hpp`): CPU use between readings of `/proc/stat`, memory in
  use against `MemAvailable`, the one-minute load, and PSI's stall percentages when the kernel
  keeps them. Readings past steve's limits (load average, minimum available memory) are drawn
  in the bad tone, since that is when steve holds jobs back.
- Under the emerge running most of it, the merge list (mtimedb's `resume.mergelist`, which
  emerge shrinks as packages merge) is a tree: each package under the one it waits for that
  merges last before it, what runs now on top with its spinner, the rest marked ready or with
  how many they wait for. What each waits for comes from `egraph-build --pending`
  (`builder/egraph_build/pending.py`), once per new list: its DEPEND, BDEPEND, RDEPEND and
  IDEPEND (not PDEPEND) reduced under the USE it is built with, matched against the rest of the
  list. Waits on packages later in merge order are left out, as emerge's order breaks those
  cycles; within an any-of group every pending member counts, since which one the resolver
  chose is not recorded. A failed pass is shown once and not retried for the same list.
- steve's line under the graphs shows how many of its jobs are handed out and its settings,
  read each second through `stevie`'s getters (`src/steve.hpp`). Where `/dev/steve` is not
  open to the user (root, or the `jobserver` group), they come from steve's command line in
  `/proc` instead, which is stale after any live change and says so. `s` steps through the
  settings: left and right pick one, up and down (or `+` and `-`) change it by a step through
  `stevie`'s setters, at once and without confirming, since steve forgets them when it
  restarts. A step stays within what steve accepts (a load average of at least 1, min-jobs no
  higher than jobs).
- Errors and notices are dialogs over the view, which the next key closes without acting on it:
  a failed check or rebuild (the drift stays listed underneath, and `u` can try again), a
  package only the fresh build has, and warnings from opening the store (`--no-refresh` on a
  stale one), which would otherwise be printed where the interface then draws.
- A package page starts with why it is kept (`why`'s chain from a root, or that depclean would
  remove it), then any dependencies nothing installed satisfies, each with what is installed
  under its name instead. A build-time dependency (DEPEND, BDEPEND) whose package is installed
  at another version or slot, typically an autotools slot pin after automake moved on, only
  records what the package was built with: it is listed apart as "since replaced" and does not
  make the package broken. Run-time ones still do, since the package may need what it asked
  for. `egraph broken`'s human layout makes the same split (`query::replaced`); its lines layout
  stays portage's plain unsatisfied set.
- A package page lists what it depends on, then what needs it, one row per package and atom
  with the kind matrix, as `deps` and `rdeps` do. Enter follows a row to that package's page, and
  the title bar keeps the trail; Esc or Backspace go back.
- Rows unfold in place (Space, Tab, or Right) into their own links in the same direction, so
  either tree can be walked without leaving the page. A package already on the path from the
  page down to a row is marked as a cycle and does not unfold. Left folds a row, climbs to its
  parent, and from the top level goes back.

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
- The atoms of one dependency list (one kind of one package, or the `||` groups and virtuals
  deferred from it) select together, as `_minimize_children` does: each selects its highest
  installed match, and where they select several packages of one cp, the lowest go first while
  every atom matching them matches another. Under dynamic deps this is common: the ebuild's
  `lib:=` beside the `lib:1/1=` it was built with keeps slot 1 only.

`why` walks the same traversal. It answers with the dependency that keeps a package, not with
every root atom that happens to match it: an unslotted world atom matching two slots keeps only
the higher one, and the other is explained by whatever depends on it.

Dependencies are the ones emerge reads by default (`--dynamic-deps=y`): an installed package's
from its ebuild when the same version is still in its repository, from the evaluated store.
`--dynamic-deps n` reads the ones the vdb recorded at merge time instead. Every query that
follows dependencies (deps, rdeps, why, orphans, broken, affected) takes the option; the store
is opened with the evaluated trees swapped in (`with_dynamic_deps`), so the queries themselves
do not know which they read.

`deps` and `rdeps --possible` also list what the ebuilds would add with USE flags toggled,
marked with the toggles (`use=a -minimal`). The vdb keeps only reduced strings, so these come
from the ebuild's, and need dynamic deps. The builder finds each chain of USE conditionals
with portage's `paren_reduce` (deprecated, wrapped in one place) and lets `use_reduce(subset=)`,
the call depgraph uses for test dependencies, decide what the chain selects. Only toggles a
user can make count: flags in the ebuild's IUSE that the profile does not mask (to turn on) or
force (to turn off); arch, elibc and other implicit flags never do.

`updates` lists what `emerge --update @installed` would replace: @installed holds a slot atom per
installed package, so each is weighed against the best visible version in its slot (of one
version, the repository of highest priority). It is replaced when that version is newer, or
when its own version has no visible ebuild (depgraph's `_equiv_ebuild_visible`: the one in its
repository, else any), which can mean a downgrade. `-N` (`--newuse`) and `-U` (`--changed-use`)
add rebuilds of the same version for changed USE, with depgraph's `_reinstall_for_flags` and
the flags in emerge's notation (`flag*` changed, `flag%` new in IUSE, `(-flag%)` gone from it).
The builder decides all of it; the C++ side plans the merge as `emerge -uD` would (`plan.cpp`,
roadmap 14b). Each installed package in scope takes its target, or falls back to the best
visible version in its slot that no member of the plan rejects, or stays put. A member's
dependency (a kept package's own, read with or without `--dynamic-deps`, or a merged
candidate's, reduced under the USE it would be built with) must be satisfied by the plan: by a
package it keeps or merges, a slot operator's sub-slot aside (see below), or else by pulling in the best visible version that matches into a slot nothing
occupies (a new package, with the dependency that pulled it). A `||` takes its first
alternative that can be satisfied so. When nothing can satisfy a dependency, the installed
package it would take away is rejected, or else the target that (through what it pulls in)
needs it, and the choice moves down a version; the plan repeats until nothing moves. A held
rebuild for USE stays put. `--held` adds each held update once, with the members and atoms that
hold it; the human layout lists them under a "Held back" heading and new packages under "New".
Each held update also gets its remedies (`remedy.cpp`, roadmap 12d). Its holders are the
installed packages among what rejects it, a rebuild's ebuild standing for the installed package
it rebuilds; each is listed with the installed packages depending on it and the root atoms
selecting it. When every holder is a leaf (nothing else installed depends on it, and no root set
but @selected keeps it), the plan is made again without them, in the scope depclean would keep
then (under `--world`), with the held package in scope so its own dependencies still weigh.
If that plan lets the update through, removing them is offered (`--deselect`, `-C`, then the
update with `-1`), with the other held updates it frees. When only holders reject it, `emerge -1
--nodeps` is offered too, with the warning that a later `-uD` undoes it. egraph prints the
commands; it never runs emerge. `test_removals_let_updates_through` asks emerge on each scenario
without the holders.
A kept dependent whose slot-operator binding (`:0/3=`) a merge in another slot or sub-slot
breaks is rebuilt from a visible ebuild of its version (roadmap 14c), and that ebuild's
dependencies join the plan like a merge's; with no such ebuild the binding holds the merge back
like a bound, so it falls back to a version in the bound sub-slot. The line names the merge the
rebuild is for.
Blockers are weighed once the plan settles, as emerge's `_validate_blockers` weighs them after
its graph is complete (`blockers.cpp`, roadmap 16d): emerge does not backtrack over a blocker,
it uninstalls a package or refuses the plan. An installed package's run-time blockers and a
merge's of every kind count. A merge's blocker on a package staying installed uninstalls that
package, and an installed package's blocker on a merge uninstalls the holder, unless the
package is in emerge's completed graph (reached from the root sets, the arguments or a merge, an
atom taking the best version left, a `||` its first alternative left satisfied) or, on the
running root, the blocker is strong. A blocker on a version a merge replaces needs nothing, and
one in a merge's own slot is ignored unless strong. What is left, and a blocker between two
merges, is a block, and the plan's exit status says emerge would refuse it. The one place
emerge looks ahead is `-u`'s greedy slots: an installed slot whose best version and the atom's
best block each other is left out, so it is uninstalled rather than updated.
What emerge refuses for dependencies nothing satisfies (roadmap 16e1) follows its backtracking
over them: a candidate the plan pulled in whose dependency no visible version matches is masked,
and what pulled it chooses again, down to its next version or, with none, to its own rejection;
an update's own merge failing holds the update back, its reason followed through the masked
candidates to the dependency at the chain's end. Only what emerge must merge refuses the plan: an
argument no version of which is left (plain emerge's with no visible version at all, even
installed), and under `-uD` a kept package the arguments reach with a run-time dependency nothing
matches; for the world sets' packages, whose missed updates emerge skips, only one that something
would match without its USE dependencies. The builder masks an ebuild with invalid metadata as
depgraph does (`masks.invalid_ebuild`), though the repository's `match-visible` counts it
visible.
REQUIRED_USE (roadmap 16e2) is checked where depgraph checks it: as it selects an ebuild, before
that ebuild's dependencies, and a failure refuses the plan with no backtracking and no say in `||`
choices. So every version a pass merges or pulls in counts, even one later given up, with two
limits that follow emerge's walk. An update is selected only through an atom that matches it (an
argument's, or a dependency's of a candidate or of a kept package within `-D`'s reach), not when
the atom reaching its package rejects it. And a pull counts only when emerge gets to it before a
dependency nothing satisfies stops it: depgraph walks RDEPEND, IDEPEND, PDEPEND, DEPEND, BDEPEND
in turn, an unsatisfied atom first within each (`_minimize_children`), and `||` groups after the
rest. Across packages egraph does not follow emerge's order, so it may refuse for a version emerge
never gets to. The check is portage's `check_required_use` ported (`src/required_use.cpp`,
portage's tree and its re-parenting in an arena), on the tokens the builder stores and the USE
the candidate has, and `test_required_use.py` holds it to portage's over generated strings and
every string in the live repositories.
A candidate's dependencies under USE other than its own (roadmap 16e3) come from its stored
tokens, reduced by portage's `use_reduce` ported (`src/use_reduce.cpp`, with the atoms' USE
conditionals evaluated as `Atom.evaluate_conditionals` prints them) and laid out as the builder
lays out its node lists; `test_use_reduce.py` holds it to the builder's reduction over generated
strings, every scenario candidate under every USE of its IUSE, and the live repositories'
conditional strings, and checks that each candidate's own USE gives back its stored nodes.
A USE dependency that no visible version meets as it would be built is met as emerge's
autounmask meets it (`_pkg_use_enabled` with a target USE): on the best visible version whose
IUSE has every flag the dependency sets (or a default for it), none it must change fixed by the
profile, and no change already asked of it contradicted; inside a `||`, only when no alternative
does without, and then on the first that can. The planner records the change and plans again
on the evaluated store with it made (`with_use_changes`: the USE replaced, the dependencies
reduced from the tokens under it and matched against the installed packages), as emerge
restarts with its config changes, until no more are asked; an installed version is rebuilt with
the changed flags. The plan keeps the changed store for its output, and is refused with the
`package.use` lines, written as emerge writes them (`>=cpv` unless something newer is visible
or installed, `>=cpv:slot` unless in its slot, else `=cpv`). The builder reaches what such a
change could pull in: a candidate's tokens reduced with the flags the reached atoms' USE
dependencies ask of its cp.
`--verify` compares the changes emerge asks for (package and flags, as sets: emerge prints a
set's order) and nothing else then, since emerge's merge list may predate some of them. Run from
a terminal, `plan` and `updates` offer to write the lines (`package_use_text`, the "# required
by" comments included) to `package.use`, or `package.use/zz-autounmask` when it is a directory,
and on yes reload the session, so the stores refresh for the changed configuration, and plan
again. Shell and interface lines never ask: their standard input is the shell's.

`updates --world` plans `emerge -uD @world` instead (roadmap 12c): the packages depclean keeps
are in scope, and one outside keeps its version while its dependencies weigh nothing. emerge's
arguments are the root sets' atoms here, not a slot atom per installed package, and only an
argument is updated regardless of what depends on it. So a `||` whose installed alternative an
update would take away rejects that update when no root atom names the package, rather than
switch alternatives. And a kept dependent bound by a slot operator to an older slot is rebuilt
against the best visible version in a newer one, as `--rebuild-if-new-slot` (depgraph's
`_slot_operator_update_probe`): when its ebuild's atoms naming the cp name no slot, the version
is higher than the bound one, and every other kept dependent's atom (a slot operator's whatever
its slot) and root atom of the bound package accepts it. That slot's installed package is
updated to it even when nothing kept it; with none installed, it is pulled in. Under
@installed every package's slot atom is an argument, so neither applies. A root atom whose best
visible match lies in a slot where nothing is installed pulls that version in (a new slot of an
unslotted atom, `sys-devel/gcc` in @system), named by the root atom. And a merge's dependency
that only a newer version of an installed package satisfies, where that package has no update of
its own (it is out of scope), replaces it with the best visible version that matches, rejected
in turn with the merge that needed it; never a downgrade.

Without `-D` (`--deep`), `updates` plans plain `emerge -u` instead (roadmap 16a), the update
most people run. Only emerge's arguments take their pending update (every installed package
under @installed, the root atoms' matches under @world), and only its target: an update
anything rejects is dropped rather than fall back, and a slot-operator binding it breaks holds
it rather than rebuild the dependent (emerge only rebuilds installed packages it goes deep
into). The rule for a merge's dependency that only a newer installed version satisfies covers
every package outside the arguments here; only the merge is listed as held when it fails. Kept packages' dependencies weigh only as emerge's completed graph weighs
them: one a merge would break rejects the merge, and one already missing stays missing,
pulling nothing in. No dependent is moved to a newer slot.

`plan` takes any request (roadmap 16b): atoms, `=cpv`, slot atoms and the sets the store holds
(`request.cpp` expands them, and fills in the category of a bare name from the cps the stores
know). The atoms become the planner's arguments in place of the root atoms, and emerge's three
ways to pick an argument's package apply: `-u` updates each installed slot an atom matches
(greedy slots) to the best version the atom accepts, keeping an installed version that is at
least as good, and adds the best version's slot; plain `emerge` merges the best version even
when it is installed; `-n` does so only when nothing emerge can keep (unmasked; a missing
ebuild does not matter) matches. What an argument must merge, as plain `emerge` does and `-u`
does for an atom the installed version fails, backtracks to the other versions the atom
matches, down to reinstalling the installed one, and rebuilds what binds to it; an atom named
alone under `-uD` keeps its installed version instead, neither falling back nor rebuilding. The
scope is @world's, as emerge completes its graph with it, plus what the arguments reach through
their dependencies (and those of the visible versions they match and would move to); with `-D`
only that reach is updated, and outside it kept packages only weigh, a slot-operator rebuild
taking the best version in its slot and leaving build-time bindings alone.
`test_plans_are_emerges` holds every scenario's cps, `=cpv`s, slot atoms and root sets to
`emerge -p`, `-pu`, `-puD` and `-pn`; four `-uD` atoms in `slotops`, whose reach needs
slot-operator rebuilds outside it that emerge's backtracking drops whole, still differ.
A name without a category is looked up among every repository cp and the installed ones, as
emerge's `_dep_expand` does, taking the one outside virtual, acct-group and acct-user when the
others share it. An argument whose cp only the repositories know (no candidates yet) is
evaluated first by `egraph-build --evaluate` into the store this user may write, then planned;
the comparison runs with every cp evaluated so.

`updates -t` lists the plan in merge order (`order_merges` in `plan.cpp`, roadmap 14d), each
merge numbered and followed by the places of the earlier merges it waits for: those its DEPEND,
BDEPEND, RDEPEND or IDEPEND match (every member of a `||`, as `pending.py` counts them; PDEPEND
never). A merge goes once all it waits for has gone, earlier plan order first; where a cycle
leaves none ready, a run-time wait is dropped before a build-time one, as emerge breaks cycles.
libc (virtual/libc's provider) and what it waits for go first, since emerge has every merge
wait for it; that wait is not listed. New packages take their place in the order instead of a
section of their own. `test_update_order_is_valid` holds it to emerge: every wait emerge's own
order keeps, egraph's keeps too.

`updates --tree` hangs each merge under the root set it comes from (`update_tree_lines`): the
chain `why` finds down to the installed package it replaces or rebuilds, and for a new package
the chain of the member that pulled it in, then the package. Chains fold into one tree per set,
children in the order the merges first reach them; merges show their versions, place and waits
as in `-t`, the packages between them plainly. Merges nothing keeps (depclean would remove the
package) come under their own heading. `test_update_tree_follows_why` holds each chain to `why`.

In removal mode `dep_zapdeps` asks which packages are *available*: an installed package whose
own metadata is masked (keywords, `package.mask`, license, invalid strings; under dynamic deps
with the ebuild's EAPI, KEYWORDS and dependencies, as FakeVartree reads them) is available only
when an ebuild of its version is visible. Such a package loses a `||` choice to an available
alternative, and among several installed matches an atom selects an unmasked one, then a
visible one (`_select_pkg_from_installed`). The evaluated store holds both bits per package; the
masks only where depclean weighs them (a package that is not visible, or one of several
installed versions of a cp), since the license check alone would add a fifth to the build. The
first of `_select_pkg_from_installed`'s filters (`package.mask` and licenses, but not keywords,
with packages already in the graph let through) is left out: it differs from the second only
when both kinds of mask meet among one atom's matches.

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
user sets directory), and the builder's own modules. A path recorded as missing is stale once
it exists.

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
configuration input or builder module, another EROOT, or a store written by another egraph or
portage version means a full build; a portage upgrade changes the vdb, so the store goes stale.
`EGRAPH_STRICT=1` compares every incremental build against a full one and fails, leaving both
old stores, on any difference.

The evaluated store is rebuilt the same way, from the vdb changes the installed build found and
its own inputs. A cp is evaluated again (candidates, dependencies, possible dependencies, update
and masks) when it gained, lost or changed an installed package, when the metadata cache
directory of its category changed in any repository, when its category directory changed, or
outside the main repository when its package directory or an ebuild changed; the other packages
only have the atoms that name a changed cp matched again. Every build lists the repositories'
cps again (170 ms on the dev box, 21,869 of them), so a category directory of no cp in the
store only makes it stale. A repository's root and metadata cache directory only list
inputs of their own, and the main repository's cache records each ebuild's eclasses, so for
those only a change of kind counts. Every other input (configuration, profiles, the user's
visibility and USE files, a repository's masks, moves, layout, license groups and categories,
another repository's eclasses), a full installed build, or an evaluated store built against
another installed store means a full evaluated build. Decided with the user: a sync
evaluates whole categories again; with several repositories, tracking single cps would not buy
much. A cp evaluated on request (`egraph-build --evaluate`, roadmap 16b2) is evaluated with what
it reaches and kept by every later build, a full evaluated build included, until `--full` (`egraph
rebuild`) or until it is installed; automatic full builds are too frequent (any configuration
edit) to drop it at.

No merge-time hook is needed, and edits to the vdb made outside portage are caught too.

## Rebuild and check

- `egraph rebuild`: full rebuild via `egraph-build --full`.
- `egraph refresh`: what every query does first, alone: an incremental build if an input changed,
  and no output.
- After an emerge that merged or unmerged anything, portage runs `/etc/portage/bin/post_emerge`
  once (upstream since 2011, though no man page says so), as root with its roots in the
  environment. That file is the user's, so egraph installs a dispatcher for it as
  `${datadir}/egraph/post_emerge`, to link there: it runs every executable in
  `/etc/portage/post_emerge.d`, as `postsync.d` does for syncs, and egraph's own entry in that
  directory runs `egraph refresh`, so the next query does not pay for the refresh (`hooks/`;
  meson options `portage_hooks` and `portage_config`). The same entry goes in
  `/etc/portage/postsync.d`, which portage runs after syncing every repository, so a sync
  refreshes the evaluated store before the next query without any setup.
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

Users who cannot write the system store still get current answers: a current system store is
read as is, and otherwise egraph keeps the user's own store,
`${XDG_CACHE_HOME:-~/.cache}/egraph/installed.egraph` (`installed-mnt-gentoo.egraph` for
`--root /mnt/gentoo`), which `rebuild` also writes. The vdb is world-readable, so the builder
needs no privileges.

egraph runs the `egraph-build` next to itself if there is one, else the one in `PATH`
(`--builder` and `EGRAPH_BUILD` override both). `meson install` puts both in `bindir` and the
package in site-packages, so the installed builder imports the installed portage. The build
directory gets a wrapper running the builder from the source tree under the `portage_lib` option
(the fork by default; empty for the installed portage), so `build/egraph` works in place.

## Consumers

Each consumer runs in shadow mode against the tool it replaces before anyone relies on it.

- CLI queries: `deps`, `rdeps`, `why`, `soname`, `broken`, `orphans`, `export`, `stats`.
- The portage fork's neighborhood completion (`depgraph._installed_graph()`), by querying `egraph`,
  which removes the 0.37 s index build from every emerge run. `egraph affected` takes one JSON
  request per root (the kinds and seeds of the reachable set, the cps being merged, the cpvs
  they replace, their blockers) and answers the reachable set, what the blockers match, and the
  affected packages, matching atoms by version and slot as the fork's `InstalledGraph` does
  (`src/affected.hpp`). The Python store reader decodes the
  live store in about 0.3 s and exists for tests only.
- The graph viewer (`~/.local/bin/portage-graph-view`) reading the store instead of building its own.
- The update fast path, once the candidate layer exists: `-uDN @world` becomes a set difference,
  and an empty difference means nothing to resolve.

## Open questions

