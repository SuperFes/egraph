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
      It does now: the first emerge after a merge that asks `egraph affected` pays the refresh,
      more than the 3G instructions egraph saves it.
- [ ] Environment variables that change implicit IUSE (`USE_EXPAND` and friends set in the
      environment rather than make.conf) are not inputs, so a change there goes unnoticed.
- [ ] `egraph` does not check the store's EROOT against its own roots; only the builder does, on
      incremental builds.
- [ ] The evaluated store tracks the main repository through its metadata cache directories, so
      an ebuild or eclass edited in place there without regenerating the cache goes unnoticed
      (portage itself would notice the stale cache entry). Other repositories track their
      installed cps' ebuilds, but not their eclasses beyond the directory.

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

- [ ] `export` neighborhoods follow dependency edges only; the fork's included blocker edges
      (without expanding them). Add them with the `blockers` query.
- [ ] Queries rebuild the edge index on every run (about 20M instructions on the live store). If
      that ever dominates, the store could carry it.

- [ ] depclean's `_select_pkg_from_installed` first keeps matches not masked by `package.mask`
      or a license (keywords do not count, and packages already in the graph pass); egraph
      goes straight to its second filter, any mask. They differ only when both kinds meet among
      one atom's installed matches.
- [ ] `orphans` ignores the rest of `dep_zapdeps` that needs more than the vdb: use.mask and
      use.force on unmet USE dependencies, package.provided, and new-style virtual expansion
      (depclean decides a virtual's `||` in its parent's context; egraph when the virtual is read).
- [ ] `orphans --ignore-soname-deps=n` (keep soname providers), as emerge offers.
- [ ] `why --all`: every chain, or every root, rather than one shortest chain.

## Updates

- [ ] `updates` lists a newer version an installed dependent's bound holds back (10 of 32 on the
      dev box: `<dev-python/astroid-4.1`, `~sys-firmware/edk2-bin-202408`, `gcr[gtk]` against
      a rebuild without gtk). The dependents' dynamic deps could name each holder
      ("held by pylint-4.0.9"), matched against the candidate with its USE; cascades (a plugin
      update needing a held one) would still need resolution.
- [ ] Candidates count an ebuild visible when `match-visible` does, but depgraph also masks one
      whose metadata is invalid (a conditional on a flag outside its IUSE, say). Package-level
      validation (`_validate_deps`) would have to run per candidate.
- [ ] New slots (`emerge -uD` pulling `foo:2` for an unslotted `foo` with `foo:1` installed)
      and slot-operator rebuilds are resolver decisions `updates` leaves out.

## Output and UX

Direction from the user (2026-09-27): output does not have to look like portage's; make it clean
and organised, and a TUI (ncurses, or Notcurses) is welcome.

- [ ] `--glyphs` defaults to Nerd Font icons; detecting a UTF-8 locale (and defaulting to
      `ascii` without one) would suit terminals that cannot show them.
- [ ] `--possible` finds each chain of USE conditionals through portage's `paren_reduce`, which
      is deprecated "without replacement". If it goes, the chains need another source; the
      semantics stay `use_reduce(subset=)`'s.
- [ ] Possible dependencies take a USE dependency's flag only where a conditional needs it:
      `x[a?]` outside any `a?` block is never listed as `x[a]` with `+a`. Only the atom changes,
      not what is depended on, except when the looser or stricter atom matches another installed
      slot.
- [ ] The TUI's rebuild runs a second full build after the check's; saving the check's own
      scratch store (copied beside the target, then renamed) would halve the wait.

## Ideas

- [ ] An in-process bash-compatible interpreter, instead of spawning bash. Nothing on egraph's
      path runs bash today: the installed layer reads vdb metadata and repos ship md5-cache. It
      would matter only if the evaluated layer generated metadata itself (overlays without a
      cache), which portage does now. Gentoo's libbash tried this and was abandoned.

## Tooling

- [ ] CLI11's CheckedTransformer error for a bad `--format` prints the enum map
      (`{dot->0,json->1} OR {0,1}`).
