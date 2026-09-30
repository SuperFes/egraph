# Vision

Where egraph could go beyond its current charter. None of this is scheduled: each idea needs
mapping out before it becomes a roadmap step, and the user decides when. Recorded 2026-09-29.
Daily use in place of emerge, the service, configuration explaining and linting, what-if and
build knowledge are now roadmap steps 16 to 20 (2026-09-29).

## A living app, and a run-and-done tool

- Interactive when the user is: the TUI whenever egraph is interacting, plain output when it is
  not (piped, scripted, hooks). Bare `egraph` on a terminal opens the living app, which stays up
  until the user quits or a fatal error; a failed command shows a message and the app carries on.
- Actions that change the system ask first by default, as `emerge -a` does; which ones is
  decided as they come up. A preview that ends in a question can replace `--pretend`: the
  user sees what would happen and says yes or no.
- The app keeps itself current: it notices changed inputs, refreshes in the background, and
  swaps the new stores in without losing the user's place. Roadmap step 13.

## What-if configuration

The loop egraph should absorb: run `emerge -pv`, read the USE flags, edit `package.use` or
`make.conf`, run it again.

- Toggle flags in the app, for one package or globally, on installed packages and ones not yet
  installed, and see the graph change: what gets pulled in, what gets rebuilt, what drops out.
  `deps --possible` and the evaluated store's candidates are the start of this.
- Switch a package's environment the same way (`package.env`, e.g. putting app-editors/ned on
  the LLVM toolchain env) and see what it affects.
- Save the changes out to the filesystem when the user accepts them, per package or global.

## A configuration space of its own

`/etc/portage` is a pile of files in several formats with overlapping precedence rules.

- egraph keeps its own organised, machine-handled configuration (USE, keywords, masks, envs,
  per package and global) and writes portage's files from it, so the user rarely has to edit
  them by hand.
- The user may still edit `/etc/portage` directly; egraph reads what is there and never
  clobbers an edit it did not make. How the two stay in sync is the first thing to map out.

## Planning, building, and a distribution

- The update set in merge order, as a tree down to each package's world root and as a table
  (`-t`), before anything builds. Roadmap step 14.
- An executor as a sibling project, not egraph itself (egraph stays read-only): portage's bash
  side (`ebuild.sh`, helpers, eclasses) kept as the phase runner, as pkgcore does, with our own
  orchestration, parallel builds with steve, merge, and a kernel-enforced sandbox (mount
  namespace plus landlock) in place of `LD_PRELOAD`. Checked by building a package both ways
  and diffing the image and vdb entry. First step: run egraph's plan through `ebuild`.
- Transactional merges: build the whole plan into a staging root (a btrfs snapshot of `/`, or
  an overlay), check it, and swap it in only once everything built, so a failure half way leaves
  the system untouched. A snapshot also gives a rollback.
- Problems presented, not panicked over: where portage refuses outright, egraph explains and
  offers to try. Example: installing dev-db/mariadb-12 over a file dev-db/mysql-connector-c
  owned made portage mask mariadb-12, though the collision only matters one way (the connector
  merges fine when mariadb owns the file). Improvements over portage still follow the
  divergence rule: a test proving egraph right, recorded in `docs/upstream-notes.md`.
- A distribution of our own on the Gentoo tree, as ChromiumOS is: a profile and an overlay for
  what differs, our tooling on top, and the packages maintained upstream.
