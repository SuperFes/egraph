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

## Store encoding (2026-09-27, roadmap step 1)

Measured on the live vdb (2,324 packages) with `perf stat -x, -e instructions:u,cycles:u`, as the
median of interleaved runs (7 for the builder, 21 for C++ loads, 9 for Python loads), warm page
cache, unprivileged user. Prototypes were throwaway scripts; the method is below so it can be
repeated.

### Builder cost split

One script run to a cutoff, so each row adds one phase to the one above.

| Phase | Δ instructions | Δ cycles |
|---|---|---|
| Import portage, load config | 0.80G | 0.49G |
| `aux_get` of 12 keys for every package | 1.93G | 0.71G |
| `use_reduce` into Atoms, parse sonames | 1.46G | 0.64G |
| `vardb.match` for every non-blocker atom (USE deps honored) | 3.96G | 1.57G |
| Total | 8.14G | 3.41G |

- 36,043 atoms after USE reduction, but only 5,679 distinct: interning atom strings pays, and
  vardb's per-atom match cache is why matching is not worse.
- Matching is half the build. An atom's matches can only change when a package of its cp is
  added, removed or changed, so incremental refresh should re-match only those atoms.

### Logical content at real size

22,891 strings, 2,431 inputs (category and package directories plus the world file), 2,324
packages, 36,975 dependency tree nodes (atoms, blockers, any-of and all-of groups), 34,647
resolved matches, 209 world atoms.

### Encodings

Both prototypes hold the same content and their C++ loaders decode to the same model (checked
by checksum). The loaders follow the project rules: bounds-checked reads over
`std::span<const std::byte>` or `std::string_view`, a file read into one buffer, strings pooled
in one `std::string`, lists in shared index vectors.

| | A: binary sections, LEB128 | B: tab-separated lines |
|---|---|---|
| Size | 1.17 MB | 1.96 MB |
| Size, zstd -19 | 307 KB | 293 KB |
| C++ decode, net of process and file read | 30M ins, 8.5M cycles | 50M ins, 13.5M cycles |
| Pure-Python decode, net of interpreter start | 750M ins, 157M cycles | 714M ins, 179M cycles |

- Both parse in a few milliseconds from C++; A is 1.6x cheaper and 40% smaller.
- In Python both take about 40 ms, versus 0.37 s for the fork's index build, so the encoding
  does not decide the Python reader question. The Python numbers are lower bounds: they
  tokenize the whole file but do not rebuild the records.
- A's section table lets the freshness check decode the header and inputs only.

Decision: A. See `store-format.md`.

## Installed layer (2026-09-27, roadmap step 2)

- `egraph-build --json` on the live vdb: 9.31G instructions (median of 7 interleaved runs),
  against 8.15G for the step 1 phases up to matching. Building the trees and the reverse index
  and writing 5.7 MB of JSON cost about 1.1G. Cycles were not comparable: the load average sat
  above 20 during the runs.
- Agreement with portage on the live vdb, every subject, no sampling: `installed`, `errors`,
  `matches` (5,679 distinct atoms), `deps` (2,324 packages), `rdeps` (34,432 edges),
  `soname_providers` and `soname_consumers` (1,001 sonames, 15,177 uses). Zero differences.
- The unsampled oracle is too slow to run routinely: `rdeps` and the soname queries rescan every
  package per subject, and the unsampled pytest run passed 10 minutes before reaching
  `soname_consumers`. The full check above inverted the oracle's per-package answers once
  instead, which is equivalent for these queries.
- use_reduce shapes the trees in ways worth knowing: a `||` left with one alternative becomes a
  plain atom, nested `||` groups are flattened, and in EAPI 7+ a `||` emptied by USE becomes the
  never-matching atom `__const__/empty-any-of` (older EAPIs drop it).

## Store reader and writer (2026-09-27, roadmap step 3)

Median of interleaved `perf stat` runs (15 for C++, 7 for Python) on the live store: 2,324
packages, 1.01 MB with inputs still empty. Load average was about 20, so cycles are noisy.

| | Instructions | Cycles |
|---|---|---|
| C++ `read_file` (process and read) | 2.7M | 4.6M |
| C++ `load`: read, decode, validate every id | 34.0M | 16.4M |
| C++ `export --format json` (5.7 MB out) | 528M | 235M |
| Python `store.decode` into an InstalledLayer, net of interpreter start | 3.1G | 1.09G |
| Python `store.encode`, per call | 0.89G | 0.49G |

- Validated decode costs 31M instructions net of the read, the same as the unvalidated step 1
  prototype, once the failure paths were moved out of line (`[[gnu::cold]]`, no inlined
  `std::format`): 45.9M to 31.3M instructions, 15.9M to 11.8M cycles.
- The byte-mutation test found a decoder bug before it shipped: after the first failure, later
  reads returned unchecked values. Hardened `.at()` turned it into an exception rather than
  memory corruption; every read now returns zero once the reader has failed.
- Python decode splits roughly into varint decoding (a third), building the index (a quarter)
  and object construction. A Python reader that materializes the store will not beat the fork's
  0.37 s index build by much; see `TODO.md`.
- The live store round-trips: `egraph export --format json` of the builder's store is
  byte-identical to `egraph-build --json` (5.7 MB).

## Freshness and incremental refresh (2026-09-27, roadmap step 4)

Live system, 2,324 packages. Instruction and cycle counts are medians of interleaved
`perf stat` runs (15 for C++, 5 for the builder); task-clock includes the kernel.

- The store records 2,575 inputs: 2,482 directories (vdb, categories, packages, profile
  directories), 92 files and 1 absent path. It grew from 1.01 MB to 1.15 MB.
- C++ load plus freshness check: 42.1M instructions and 11.6M cycles in user space, against
  38.1M and 10.4M for the load alone. Counting the kernel, the 2,575 `lstat` calls add about
  2.6 ms (task-clock 5.5 ms to 8.1 ms for the whole process).
- Builder, on a copy of the live vdb without CONTENTS and environment files:

  | Build | Instructions | Cycles |
  |---|---|---|
  | Full | 8.27G | 4.41G |
  | Incremental, nothing changed | 5.00G | 1.83G |
  | Incremental after upgrading dev-libs/openssl (199 reverse deps re-matched) | 5.05G | 2.05G |

  The openssl case re-read one package and matched a full build exactly (`EGRAPH_STRICT=1`).
- Most of the incremental floor is the Python store decode and encode. Two changes cut it before
  the numbers above: building the query index lazily (the builder never queries) and a
  single-byte fast path for varints. No-change incremental went from 6.06G to 5.00G
  instructions and 2.40G to 1.83G cycles; full builds from 8.81G to 8.27G.
- A bug the tests caught on the way: the vdb directory itself was classified as configuration,
  so a new category forced a full build.

## Queries (2026-09-27, roadmap step 6a)

Live store, 2,326 packages, 34,432 edges. `perf stat` medians of 15 interleaved runs, whole
process including load and the freshness check of 2,575 inputs.

| Command | Instructions | Cycles |
|---|---|---|
| `egraph --version` | 3.0M | 5.1M |
| `egraph rdeps dev-libs/openssl` (199 edges) | 61.6M | 40.6M |
| `egraph soname libssl.so.3` | 43.2M | 24.0M |
| `egraph broken` (156 lines) | 48.1M | 26.9M |
| `egraph export --depth 2 dev-libs/openssl` (dot) | 116.6M | 67.1M |

- `rdeps dev-libs/openssl` takes 15.5 ms of task-clock. `equery depends dev-libs/openssl` takes
  5.1 s wall.
- equery lists 104 packages and egraph 97, all of them in equery's list. The other 7 have
  openssl behind USE flags that are off in the installed build (`pkcs7?` on kmod, `utils?` on
  nghttp2): equery reads the repository's unreduced metadata, while the vdb stores dependencies
  reduced by the USE they were built with. portage's `vardb.match` agrees with egraph.
- Every `deps`/`rdeps` answer through the binary matches the oracle on every scenario and on a
  sample of 20 live packages; `broken` matches the oracle on the whole live system.

## Atom matching (2026-09-27, roadmap step 6b)

- The C++ matcher agrees with `vardb.match` on all 5,667 distinct atoms in the live dependency
  trees (1,814 with USE dependencies), on a generated corpus of 339 atoms covering every
  operator, glob, slot, sub-slot, repository and USE-default form against 20 versions, and on
  every atom of every scenario. It also rejects the 7 corpus atoms portage's `Atom()` rejects.
- Portage's `vercmp` is what the port follows, not PMS text; the differences that matter are a
  missing numeric component sorting below 0 (1.0 < 1.0.0) and `1.010 == 1.01` from its
  zero-padding of fractional components.
- Matching every live atom against all 2,326 packages in one `egraph match` run costs under 1G
  instructions; a query argument is one atom and costs nothing measurable next to the load.

## Roots (2026-09-27, roadmap step 7)

- The live system has 260 root atoms: 210 in @selected, 50 in @system, none in @profile. Three
  world atoms match more than one installed slot (ruby, lld, gentoo-sources).
- Reading the root sets costs about 130 ms warm: half is `vardb.match` on the root atoms, 30 ms
  constructing the portdbapi that portage's set configuration insists on, and the rest parsing
  it. Every build reads them, so incremental builds reuse the previous matches of root atoms whose
  cp was not touched.
- A live `emerge --depclean --pretend` takes 9 s wall and would remove 2 packages: the older
  gentoo-sources slots, which the unslotted world atom does not keep.
- `egraph orphans` agrees exactly with `emerge --depclean` on the live system: 2 packages with
  build-time dependencies kept (the default) and 358 without, against 5.6 s and 2.6 s for
  depclean's resolution alone. egraph takes 80M instructions and 27M cycles (9 ms), about what
  `stats` costs.
- Depclean's `||` choices depend on visibility, not just on what is installed: in removal mode
  `dep_zapdeps` asks the repository-backed composite db which alternatives are available. On
  playgrounds without KEYWORDS every installed package counts as masked and unavailable, so the
  first installed alternative always wins, even over one already kept. The scenarios therefore
  give installed packages an accepted keyword, the state of a live system.
- Against `--dynamic-deps=n`, which is what egraph implements, `why` gives a shortest chain of
  depclean's own parent links for every one of the 2,324 kept packages (1,968 without build-time
  dependencies). Against emerge's default dynamic deps, 1,393 packages' `>=sys-libs/glibc-*`
  runtime dependencies, present in the vdb but in no ebuild, are invisible to depclean; that
  changes paths to glibc and nothing else. `why` costs 88M instructions and 34M cycles.

## Consumers: neighborhood completion (2026-09-27, roadmap step 8)

- `egraph affected` answers everything the fork's `_complete_neighborhood` asks of its
  `InstalledGraph` (reachable, blocker matches, affected) in one call, matching atoms by version
  and slot as the fork does. On the live system it agrees with the fork on the whole vdb at once
  (every cpv as seed, changed and replaced, every blocker) and on 200 sampled packages one at a
  time; the scenarios agree for every cp, cpv and blocker.
- A realistic request (214 world packages as seeds, glib changed and replaced, runtime kinds
  and sonames): the fork's index build plus its queries cost 5.1G instructions and 3.0G cycles
  above Python and portage's own start-up (0.77G); `egraph affected`, process start included,
  costs 0.14G instructions and 0.065G cycles, about 36 times fewer instructions.
- The depgraph's vartree is a FakeVartree, which under emerge's default `--dynamic-deps=y`
  reads an installed package's dependencies from its ebuild when that version is still in the
  repository. egraph reads the vdb, as `--dynamic-deps=n` does, so the two indexes can differ
  there (see Roots: 1,393 packages' glibc dependencies are in the vdb and in no ebuild).
- The fork asks it under `PORTAGE_DEPGRAPH_EGRAPH` (commit 2ad3b002d there): `shadow` and
  `strict` compare it with the index, `on` uses it when dynamic deps are off. Its seven
  neighborhood completion scenarios pass in strict mode, and a live `emerge --pretend --update
  @world` in shadow mode asked egraph three times (once per backtrack; 298 seeds) with no
  difference, with dynamic deps on or off.
- That whole `emerge --pretend --update --dynamic-deps=n @world` costs 23.5G instructions and
  15.1G cycles with `on`, against 26.7G and 17.4G with the index: 12% of the instructions and 13%
  of the cycles of a resolution, interleaved runs agreeing within 1%.
- The graph viewer (`~/.local/bin/portage-graph-view`) builds its graph from `egraph export
  --format json` in 0.30 s wall against 1.38 s for the fork's index. Same 2,326 packages; 51,134
  edges against 51,211, the 77 missing ones all edges the index draws by ignoring USE
  dependencies: `app-alternatives/gpg[freepg(-)]` to a gpg built without freepg, or a blocker
  `!app-crypt/gnupg[-alternatives(-)]` to a gnupg with alternatives on. None is new.

## The repository side (2026-09-28, planning roadmap step 9)

- 8 repositories (gentoo, Local, cosmic-overlay, guru, kde, qt, steam-overlay, tlp): 21,861 cps
  and 38,303 ebuilds; gentoo's md5-cache is 147 MB.
- Every one of the 2,326 installed packages is still in its repository. Reduced under the
  installed USE, 1,648 of them have dependencies in the ebuild that differ from the vdb's:
  RDEPEND 1,418, BDEPEND 168, IDEPEND 125, DEPEND 103 (for example every acct-group package's
  new IDEPEND on `>=sys-apps/shadow-4.6`). Dynamic deps are the common case, not the exception.
- Warm cache: the ebuild metadata of every installed package 2.24 s; `bestmatch-visible` for all
  2,293 installed cps 1.10 s; `match-visible` for them 1.89 s (4,881 cpvs); effective USE via
  `config.setcpv` 0.54 ms per package; `cp_list` over every cp 2.45 s.
- The oracle's dynamic dependencies (`oracle.dynamic_dep_strings`, FakeVartree's rule rebuilt on
  portage's public API plus one wrapped `_pkg_str`) equal FakeVartree's own for every installed
  package in every scenario and for all 2,326 on the live system.

## Evaluated store (2026-09-28, roadmap step 9b)

- Live system: every installed package's dependencies come from its ebuild (2,326 `ebuild`,
  none `vdb` or `moved`); 4,881 candidates across 2,293 cps, none of them a masked installed
  version. 2,135 inputs. The evaluated store is 1.06 MB beside the installed store's 1.16 MB.
- A full `egraph-build` writing both stores costs 62.2G instructions against about 8.3G for the
  installed store alone. The evaluated pass is dominated by candidates: `config.setcpv` for
  effective USE is about 60% of it and visibility most of the rest, both through portage's
  public API. Visibility through a repository-qualified `match-visible` per cp and repository
  measured 1.05-1.33 s against 1.20-1.71 s for `getmaskingstatus` on every version, so the
  builder asks `getmaskingstatus` only why an installed version is masked.
- The builder matches the oracle on dependencies for a sample of 100 packages, and on visible
  candidates and the best version per slot for every installed cp; `egraph export --evaluated`
  reproduces the builder's JSON byte for byte on the live system.

## Possible dependencies (2026-09-28, roadmap step 9d)

- Live system: 1,300 of the 2,326 installed packages have possible dependencies, 11,202 entries
  in all (9,976 needing one toggle, 1,193 two, 33 three), of which 5,955 match an installed
  package (5,974 edges). The evaluated store grows from 1.06 MB to 1.14 MB (1.28 MB with
  inputs).
- `egraph rdeps --possible dev-libs/openssl` names the same 104 packages `equery depends`
  lists, which reads the repository's unreduced metadata. The 7 beyond plain `rdeps` include
  kmod (`+pkcs7`) and nghttp2 (`+utils`) from the step 6a comparison.
- The evaluated pass costs 64.2G instructions against 58.3G without possible dependencies
  (+10%). A first version set a config to every installed ebuild for its use.mask and use.force
  and cost 81.9G; the candidate pass already sets one to each installed version, so it records
  them there.
- `rdeps --possible dev-libs/openssl` costs 124.4M instructions against 121.2M for `rdeps`
  (both with dynamic deps, which load both stores).
- Every entry is held to the oracle: its toggles add it, no fewer of them do, and every edge the
  oracle adds for up to one toggle (live sample of 400 packages) or any number (scenarios) is
  listed. The binary's output equals the layer's on a live sample.

## Dynamic dependencies in queries (2026-09-28, roadmap step 9c)

- depclean selects the atoms of one dependency list together (`_minimize_children`). With the
  vdb's dependencies this never changed an answer here, but under dynamic deps every `:=` atom
  of an ebuild sits beside the built `:SLOT/SUB=` atom FakeVartree appends, and with two slots
  of the child installed the unbuilt one alone would keep the higher slot. egraph now emulates
  it; the `repository` scenario pins the case.
- A live `emerge --pretend --update @world` under the default `--dynamic-deps=y`, with the fork
  in strict mode, asked `egraph affected --dynamic-deps y` three times and found no difference
  from its own index over the FakeVartree.
- That emerge costs 26.9G instructions and 21.1G cycles with `on`, against 30.75G and 24.3G with
  the index (interleaved pairs within 0.2%): 12.5% of the instructions and 13% of the cycles,
  now under emerge's default rather than only with `--dynamic-deps=n`.

## Updates and masked installed packages (2026-09-28, roadmap step 9e)

- Live system (2,326 installed): `emerge -pu @installed` replaces 20 packages and pulls in one
  new slot as an update's dependency (`media-video/ffmpeg-chromium`). `egraph updates` lists
  29, the same 20 with the same targets and 9 more that installed dependents hold back: bounds
  (`<dev-python/astroid-4.1` from pylint, `=llvm-runtimes/libclc-22*` from mesa,
  `~sys-firmware/edk2-bin-202408` from qemu, `=cosmic-base/pop-launcher-1.9*` against the 9999
  ebuild, `<app-misc/openrgb-1.0` from its plugins), sub-slot pins (`:0/3` on
  `media-libs/libdisplay-info`, `:0/10.06=` on `app-text/ghostscript-gpl`), and the openrgb
  plugins, whose updates need the held openrgb. With `-N` emerge adds 2 rebuilds
  (gentoo-sources 7.2.6 and 7.2.7, `experimental* symlink*`) and egraph 3: the third, app-crypt/gcr
  losing `gtk`, is held by gnome-keyring's `gcr[gtk]`. `-U` finds the same rebuilds.
- emerge takes about 30 s for `-pu @installed`; `egraph updates -N` costs 91.3M instructions,
  almost all of it loading the two stores.
- One installed package has no visible ebuild of its version (an opera release since removed)
  and none has masked metadata, so `orphans` answers as before; every depclean comparison stays
  exact in both modes.
- The evaluated pass costs 65.3G instructions against 64.2G (+1.8%; the updates alone +1.1%).
  Masks for every installed package cost 78.8G (+22%), most of it `getMissingLicenses`
  expanding the license groups on every call; depclean only weighs them for a package that is
  not visible or shares its cp with another installed version, so only those get them. The
  store grows by 5 bytes or more per package, to 1.29 MB with inputs.
- Portage details the scenarios pinned: depgraph masks an ebuild with invalid metadata (a
  conditional on a flag outside IUSE) that `match-visible` counts as visible; of one version in
  two repositories emerge weighs the higher priority's (`getRepositories()` lists repositories
  highest priority first); an installed version whose ebuild is gone is replaced even by an
  older visible one; under dynamic deps FakeVartree reads KEYWORDS from the ebuild, so an
  installed package built from `~x86` is unmasked once its ebuild is stable.

## Incremental evaluated store (2026-09-29, roadmap step 9f)

- Before, every `egraph-build --incremental` rebuilt the evaluated store in full, so the
  post_emerge refresh cost 70.5G instructions even with nothing changed (a full build of both
  stores takes 10.7 s).
- Live system, both stores incremental, with `EGRAPH_STRICT=1` passing in every case:
  nothing changed 10.3G; one package changed in the vdb (dev-libs/openssl, which many packages
  depend on) 11.9G; dev-python's metadata cache changed (172 installed cps) 15.1G; sys-libs's
  11.8G. The floor is decoding and re-encoding the two stores in Python, about 4 s of the 6 s
  profiled; rematching atoms when nothing was touched cost another 1.2G until skipped.
- A sync regenerates the cache entries of whatever changed, which renames files into their
  category's cache directory, so category granularity is what the directory mtimes give.
  Testing it needs manifests updated with the cache (`egencache --update --update-manifests`):
  egencache skips an ebuild whose digest does not match, and portage then regenerates the
  metadata itself, which is the in-place edit TODO.md records as unnoticed.


## Candidates' own dependencies (2026-09-29, roadmap step 14a)

- Dev box: 5,023 candidates (every visible version of the installed cps, and of the cps the
  dependencies would pull in), 36 cps beyond the installed ones, `dev-cpp/mm-common` among
  them. The closure itself costs 0.27 s.
- The evaluated store grows from 1.29 MB to 1.99 MB. A full build of both stores: 69.6G to
  78.2G instructions (+12%).
- An incremental build with nothing changed: 10.3G to 15.4G, then 14.5G once candidate trees
  are named while decoding rather than through `_replace`. The rest is decoding and encoding
  the extra nodes in Python, the floor recorded under step 9f.
- With every `||` alternative an update could break in the closure (step 14b): 5,079
  candidates, 54 cps beyond the installed ones (podman's stack, rust, openjdk, perl-core among
  them, through `||` groups whose installed choice has a newer version outside the bound). A
  full build of both stores: 81.3G instructions.

## Repository cps in the evaluated store (2026-09-30, roadmap step 16b2)

- Dev box: 21,869 cps across 8 repositories; `cp_all()` lists them in 170 ms. The evaluated
  store grows from 2.01 MB to 2.48 MB, and its inputs from 2,159 to 2,394 (every repository's
  category directories).
- `egraph updates` on its own store (`--store`): 255.3M to 269.0M instructions (+5.4%),
  decoding the strings and the list.
- Without `--store`, a user's run first tries the system store; a system store of the older
  format costs the whole installed store's decode (36.6M instructions) before the evaluated
  store's version rejects it, until the system package is upgraded too.

## Parallel exec beside emerge --jobs (2026-10-02, roadmap step 16k4c)

- Playground: 8 independent packages, each compiling for 2 s (`sleep`), merged from the same
  snapshot by `egraph install` (real emerge), `egraph exec` and a bare `emerge -1`, three
  interleaved rounds each; wall clock, since the work is mostly waiting.

  | jobs | install (emerge) | exec | bare emerge |
  |------|------------------|------|-------------|
  | 1    | 42.7–43.5 s      | 43.6–44.3 s | 40.9–41.8 s |
  | 4    | 19.0–20.4 s      | 19.3–20.0 s | 18.2 s      |

- exec keeps pace with emerge at both job counts. Its pool's workers each load portage's
  configuration (about a second) in parallel with the first builds, so the extra start-up
  hides behind them. The 1–3 s on a bare emerge is egraph's planning, verification against
  `emerge --pretend` and store refresh, which `install` and `exec` share.
- The first measurement had emerge at 47 s with `--jobs=4`: emerge's `_can_add_job` starts
  a build beside others only while PORTAGE_TMPDIR has `--jobs-tmpdir-require-free-gb` (18
  GiB, and 1 GiB per running build) free, and the playground's /tmp has 16 GiB. exec now
  holds builds back the same way; the comparison above sets the option to 0 for both.

## The builder suite in parallel (2026-10-05)

- 3943 tests on the fork, on this box's 8 cores (16 threads): 699 s serially. With
  pytest-xdist's `-n auto` (8 workers), 313 s distributing by test (`--dist load`), 338 s by
  file, 155 s with `--dist worksteal`; 16 workers (`-n logical`), 162 s.
- `load` first hands out contiguous chunks, so `test_exec_run`'s 20 tests (260 s of the 695)
  all went to one worker, which ran 308 s while the others were done by 150. With work
  stealing every worker is busy 125–159 s of the 161.
- Each test runs about 1.6 times slower beside 7 others (1075 s summed against 695): the
  playgrounds' builds and merges compete for the cores, so 16 workers gain nothing.
- Under 16 workers the TUI tests caught Notcurses partway through a redraw; their checks now
  wait for everything they read.

## The repository index (2026-10-06, roadmap step 16i)

- This box: 8 repositories, 21,875 cps, 38,544 ebuilds; five overlays (tlp, steam-overlay, qt,
  kde, cosmic-overlay) carry no metadata cache.
- `egraph-build --repository`: 15.7 s with warm caches (125 MB peak), aux_get of eight keys
  for every ebuild dominating. The file is 5.5 MB: strings 3.56 MB (descriptions, homepages,
  cpvs), versions 1.38 MB, inputs 0.61 MB (9,139 paths, mostly the cacheless overlays'
  package directories and ebuilds), the visibility configuration 9 KB.
- 525 ebuilds have a conditional in LICENSE and 23 in PROPERTIES, the only ones whose
  visibility needs their USE; the builder computes it for those alone.
- `egraph export --repository` decodes it and writes its 14.7 MB of JSON in 82 ms.
- ACCEPT_LICENSE as portage expands it changes order between runs (a group's members come
  from a set); the index keeps its net effect instead.
- `egraph complete` on the stores alone: 27 ms a key press with all 21,875 repository cps.
- `egraph versions`, every version's visibility and reasons: 2.4 s at first, 20 G
  instructions, most of it every package.mask atom (thousands, stacked from the profiles)
  matched against every version. Filing the atoms by cp and keeping the global accept lists'
  net effect, copied only for a package a package.* line names: 72 ms, 0.75 G instructions,
  the load and the output included. All 38,544 agree with portage.
- Evaluated from the visibility ledger, stacked in C++ at every load instead of read as
  portage's net lists (repository index format 6), on 38,837 versions with output unchanged:
  0.78 G instructions against 0.74 G, the stacking's 41 M (8 repositories, 344 repository and
  33 profile masks) the whole difference, of which the mask stack's linear lookups are 12 M;
  `egraph versions python` 0.26 G against 0.22 G. At first 0.95 G: the stack keeps a
  package.keywords layer per profile node, and looking each empty one up for every version
  cost 170 M until empty layers were dropped.
- An incremental `egraph-build --repository` with nothing changed: 1.8 s (interpreter and
  portage start-up, 9,139 stats, the previous index decoded), against 15.7 s for a full one.
- Depgraph's validity check (`masks.invalid_ebuild`) on every ebuild: 0 of 38,544 invalid on
  this box, but it reads every dependency string and parses it, taking a full index build from
  15.7 s to 36.8 s. Full builds are rare: an overlay's eclasses now re-read that overlay alone,
  and only a profile, make.conf or builder change reads everything again.
- `egraph search openssl`: 78 ms, `search -S toolkit`: 99 ms, against 0.83 s and 1.52 s for
  emerge --search. Both agree on all 1,399 and 2,821 lines of a dozen keys.

## Planning beside emerge on the dev box (2026-10-06, roadmap step 16k8)

- The same request on the live system (106 upgrades, 7 held), `/usr/bin/time`, warm caches;
  `--verify` agrees on the merge list.

  | command                          | wall    | user CPU | max RSS |
  |----------------------------------|---------|----------|---------|
  | `emerge --jobs=8 -uDNpv @world`  | 88.8 s  | 88.2 s   | 424 MB  |
  | `egraph updates -D -N --world`   | 0.35 s  | 0.34 s   | 35 MB   |

- egraph answers from fresh stores. Bringing them up to date is the builder's: 12.4 s
  (11.9 s user, 202 MB) for `egraph rebuild` from scratch, 0.05 s for a `refresh` with nothing
  to do; the portage hooks run it after every emerge and sync, off the query's path.
- Builds were not timed live: with ccache, whichever tool builds second compiles warm. 16k4c's
  playground comparison (above) is the scheduling one.

## GLSAs in the repository index (2026-10-07, roadmap step 17e1)

- 3,853 GLSAs in the main repository. Portage's `glsa` module parses them all in 1.8 s
  (minidom), which a full index build adds and an incremental one skips unless their directory
  changed. They take 0.96 MB of the index (now 6.5 MB).
- `egraph notices` with the GLSAs matched: 0.43 s, nearly all of it `egraph-build --notices`;
  none affect this box, as `glsa-check -t affected` agrees.
- A generated corpus of 240 GLSAs on each of the 27 scenarios (every range kind, slots, arch
  rules, applied ones) finds 30 to 605 affected packages each; egraph and the module agree on
  all.
