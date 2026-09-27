# TODO

Finer-grained than `docs/roadmap.md`: loose ends, open questions and test gaps. Delete items when
they land; move anything that becomes a plan into the roadmap.

## Oracle (`builder/egraph_build/oracle.py`)

Portage's answers, computed with no index. Every query egraph answers needs one here first.

- [ ] `broken`: needs `||` satisfaction. Check whether `portage.dep.dep_check` with the vardb can
      answer it directly before composing it from `matches`.
- [ ] `blockers`: per parent, strength and the installed packages each blocker matches.
- [ ] Roots: world and `world_sets` from `portage._sets`, @system and @profile through the profile.
      Check whether the sets API counts as public enough or needs wrapping in one place.
- [ ] `orphans`: compare against depclean. `ResolverPlayground.run(..., {"--depclean": True})`
      gives a cleanlist on playgrounds; on the live system, `emerge --depclean --pretend`.
- [ ] `why`: shortest path from a root; define tie-breaking so answers are comparable.
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

- [ ] Matching is half the build (`findings.md`). Incremental refresh should re-match only atoms
      whose cp had a package added, removed or changed, not every atom of every package.
- [ ] Measure the stat-on-load cost in C++ for the 2,431 live inputs (design assumes a few ms).

## Store

- [ ] If the Python reader (portage fork consumer) proves slow, store the large lists as
      fixed-width u32 columns: Python can view them with `memoryview.cast` instead of decoding
      varints one by one. The step 1 Python numbers only tokenized, so they are lower bounds.

- [ ] The canonical JSON carries strings as portage returns them; a non-UTF-8 byte in a path or
      atom arrives as a lone surrogate. Decide how the store and the C++ JSON export spell it
      before the step 3 golden test.

- [ ] Record the profile's implicit IUSE settings in the store for the C++ atom matcher, and the
      profile files they come from as inputs. Check how `settings._iuse_effective_match` builds
      its pattern before choosing a representation.
- [ ] Prefix installs: the default store path is `${EROOT}/var/cache/egraph/`, but only the
      builder knows EPREFIX. `egraph` could take it from the store it finds, or from `--eprefix`.

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
