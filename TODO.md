# TODO

Finer-grained than `docs/roadmap.md`: loose ends, open questions and test gaps. Delete items when
they land; move anything that becomes a plan into the roadmap.

## Oracle (`builder/egraph_build/oracle.py`)

Portage's answers, computed with no index. Every query egraph answers needs one here first.

- [ ] `broken`: needs `||` satisfaction. Check whether `portage.dep.dep_check` with the vardb can
      answer it directly before composing it from `matches`.
- [ ] `blockers`: per parent, strength and the installed packages each blocker matches.
- [ ] Oracle `rdeps` costs about 2.3 s per package on the live vdb (2,324 packages), which is why
      the system comparison samples. If that hurts, evaluate every package's edges once per vardb.

## Scenarios (`builder/tests/scenarios.py`)

- [ ] Older EAPIs: no BDEPEND/IDEPEND, no slot operators, EAPI 0 without slots in atoms.
- [ ] Repository deps (`::gentoo`) and packages from more than one repo.
- [ ] USE_EXPAND flags (`python_targets_*`) and implicit IUSE (arch, `prefix`, USE_EXPAND_IMPLICIT):
      USE deps on flags outside the ebuild's IUSE depend on IUSE_EFFECTIVE, not IUSE.
- [ ] `virtual/*` packages and new-style virtuals depending on each other.
- [ ] Several slots of one cp, and several versions a single atom matches.
- [ ] Weak and strong blockers that match installed packages, including self-blocks. The layer
      records every package a blocker's atom matches, the parent included; what counts as a
      conflict is for the blockers query to decide.
- [ ] Dependency cycles within one kind and across kinds.

## Freshness and incremental refresh

- [ ] An incremental refresh after a one-package upgrade costs 5.0G instructions against 8.3G for
      a full build, and nearly all of it is decoding and re-encoding the store in Python
      (`findings.md`). If refresh latency after a merge matters, that floor is the thing to cut.
- [ ] Environment variables that change implicit IUSE (`USE_EXPAND` and friends set in the
      environment rather than make.conf) are not inputs, so a change there goes unnoticed.
- [ ] `egraph` does not check the store's EROOT against its own roots; only the builder does, on
      incremental builds.

## Store

- [ ] Root sets that portage cannot load (a world_sets entry naming a missing set) fall back to
      the set's own atoms, as depclean does, but depclean then refuses to run. The store does not
      record that the sets were broken, so `orphans` cannot refuse too.
- [ ] Sets whose atoms come from the vdb or the repository (`@installed`, `@live-rebuild`, ...)
      named in world_sets are read again on every build, but a repository change alone does not
      trigger one.

- [ ] A libFuzzer target for `decode()`. The unit tests mutate every byte of a small store, but
      the entry point takes a pointer and a size, so it needs a reviewed exception to
      `-Wunsafe-buffer-usage` in that one file.

- [ ] Implicit IUSE patterns for pre-EAPI-5 packages are stored as literals and `x_.*` prefixes.
      Portage compiles them into one regex unescaped, so a flag containing a regex character
      (`+` is legal in flag names) would match differently. None exist on the dev box.

## Queries

- [ ] `broken` lists every kind, and on the live system most of its 156 lines are BDEPEND on build
      tools removed since (automake 1.18). Decide whether it defaults to runtime kinds
      (RDEPEND, PDEPEND, IDEPEND) with a `--kind` filter, as `--with-bdeps` does for depclean.
- [ ] `export` neighborhoods follow dependency edges only; the fork's included blocker edges
      (without expanding them). Add them with the `blockers` query.
- [ ] Queries rebuild the edge index on every run (about 20M instructions on the live store). If
      that ever dominates, the store could carry it.

- [ ] `orphans` takes every installed package as visible. A masked installed package without a
      visible ebuild is unavailable to depclean's `||` choice and its multi-slot preference; the
      evaluated layer (step 9) can store a visibility bit. Add a scenario with such a package then.
- [ ] `orphans` ignores the rest of `dep_zapdeps` that needs more than the vdb: use.mask and
      use.force on unmet USE dependencies, package.provided, and new-style virtual expansion
      (depclean decides a virtual's `||` in its parent's context; egraph when the virtual is read).
- [ ] `orphans --ignore-soname-deps=n` (keep soname providers), as emerge offers.
- [ ] Dynamic dependencies (`--dynamic-deps=y`, emerge's default) for `orphans` and `why`: read
      an installed package's dependencies from its ebuild when the same version is still in the
      repository. Needs the evaluated layer's metadata.
- [ ] `why --all`: every chain, or every root, rather than one shortest chain.

## Output and UX

Direction from the user (2026-09-27): output does not have to look like portage's; make it clean
and organised, and a TUI (ncurses, or Notcurses) is welcome.

- [ ] `broken` in the human layout is 156 entries on the dev box, nearly all BDEPEND on removed
      build tools; a runtime-only default (see Queries) would make it readable.
- [ ] `--glyphs` defaults to Nerd Font icons; detecting a UTF-8 locale (and defaulting to
      `ascii` without one) would suit terminals that cannot show them.
- [ ] Dependencies behind disabled USE flags, opt in and marked with the flag that would pull
      them in (`rdeps --possible`). The vdb only keeps reduced strings, so this needs the
      unreduced ones from the repository's metadata cache: the evaluated layer (step 9).
- [ ] TUI, next steps: a dependency tree that expands and collapses (deps and rdeps); `why`
      and `orphans` views; `broken` and `check` panels; dialogs for errors; later, possibly the
      build itself. Marks on list rows (root set, orphan, broken) would suit the list too.

## Ideas

- [ ] An in-process bash-compatible interpreter, instead of spawning bash. Nothing on egraph's
      path runs bash today: the installed layer reads vdb metadata and repos ship md5-cache. It
      would matter only if the evaluated layer generated metadata itself (overlays without a
      cache), which portage does now. Gentoo's libbash tried this and was abandoned.

## Tooling

- [x] pytest is only installed for python3.13 here, not 3.14: install `dev-python/pytest` for
      3.14, or make meson pick an interpreter that has it.
- [ ] Install `egraph-build` through meson so `egraph` can find it without PYTHONPATH.
- [ ] CLI11's CheckedTransformer error for a bad `--format` prints the enum map
      (`{dot->0,json->1} OR {0,1}`).
