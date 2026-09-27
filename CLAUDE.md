# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

egraph keeps a persistent, always-available dependency graph of a Gentoo system and answers queries
against it in milliseconds (reverse deps, why-installed, soname consumers, broken deps, orphans).
It is a standalone tool: it never patches portage. Portage is used read-only, only to build the
store, and doubles as the correctness oracle.

Read `docs/design.md` before changing architecture, `docs/roadmap.md` for what step is current,
and `docs/findings.md` for the measurements the design rests on.

## Architecture

Two halves with a hard boundary, the store file between them:

- **`egraph`** (C++23, `src/`): loads the store, checks freshness, answers queries. Never imports
  or embeds Python, never reimplements portage semantics it cannot verify.
- **`egraph-build`** (Python, `builder/egraph_build/`): imports portage read-only, evaluates the
  installed packages, writes the store atomically. Slow but rare (after sync, merge, config edit).

The store format is specified in `docs/store-format.md`; both sides must match it exactly and the
format version must be bumped on any layout change. Cross-language golden tests are the contract.

Ebuild semantics (dependency strings, atoms, USE, EAPI rules, profiles, the vdb layout) come
from portage's own API, never from our own parsing: the ebuild ecosystem is too large to
reimplement. The C++ side reads only what the builder already resolved.

Portage reference: the fork at `/Development/Gentoo/portage` (branch `depgraph-trail-shadow`).
`lib/portage/dbapi/_InstalledGraph.py` there is the reference implementation of the installed
layer; port its semantics, then extend them (see roadmap).

## Commands

Keep this section in sync with reality.

```sh
meson setup build && meson compile -C build          # egraph binary
meson test -C build --print-errorlogs                 # Catch2 + pytest
meson setup build-san -Db_sanitize=address,undefined -Db_lundef=false && meson test -C build-san
clang-tidy -p build src/*.cpp                         # safety checks, config in .clang-tidy
clang-format -i src/*.cpp src/*.hpp tests/*.cpp tests/*.hpp
black builder
PYTHONPATH=/Development/Gentoo/portage/lib pytest builder/tests   # builder against the fork
EGRAPH_SYSTEM_TESTS=1 PYTHONPATH=... pytest builder/tests         # also compare on the live vdb
```

`meson test` puts the `portage_lib` option (default: the fork) on PYTHONPATH; the playground
tests need a portage checkout for its test GPG keys.

Toolchain on this machine: clang 23, gcc 16, meson 1.12, Catch2 3.15, CLI11 2.7, Python 3.14.

## Testing against portage

`builder/egraph_build/oracle.py` answers each query the slow way, straight from portage (vardb
matching, `use_reduce`), with no index. `builder/tests/compare.py` runs a query through the oracle
and through egraph and diffs them over every package, atom and soname in a scenario. Scenarios in
`builder/tests/scenarios.py` are `ResolverPlayground` systems; every comparison runs on all of
them. `test_oracle.py` pins the oracle to hand-worked expectations. Unimplemented egraph queries
are strict xfails that must flip when the implementation lands.

## C++ rules: memory safety is not negotiable

No memory-unsafe code. Templates and values first, smart pointers only when ownership genuinely
needs the heap.

- No `new`/`delete`, no `malloc`, no owning raw pointers. `std::unique_ptr` for single ownership,
  `std::shared_ptr` only when shared ownership is real.
- No C arrays, no pointer arithmetic, no indexing through raw pointers. Use `std::array`,
  `std::vector`, `std::span`. clang's `-Wunsafe-buffer-usage` is an error in this project.
- No `reinterpret_cast`, `const_cast`, C-style casts, unions (use `std::variant`), varargs, or
  `mem*`/`str*` C functions. Narrowing conversions go through a checked helper.
- Binary decoding reads from `std::span<const std::byte>` with bounds checks and `std::bit_cast`;
  never overlay structs on bytes and never `mmap`. Files are read into a `std::vector<std::byte>`.
- `std::string_view` and `std::span` are parameters, not storage. A member or return value that
  views another object needs `[[clang::lifetimebound]]` and a lifetime a reviewer can see.
- Do not hold iterators or references into a container across a mutation of it.
- Hand `argc`/`argv` straight to CLI11; never index `argv` (that is pointer arithmetic).
- C APIs (process spawning, anything `std::filesystem` does not cover) live only in `src/os.cpp`
  behind value-typed wrappers. If a task seems to need an unsafe construct, stop and ask.
- Errors are values: `std::expected` for anything fallible (I/O, parsing, spawning). Exceptions only
  for programmer errors.
- Static dispatch by default (templates, `std::variant`, plain functions). An interface only when
  the call site genuinely cannot know the concrete type, and never per element in a hot loop.
- Builds use `-D_GLIBCXX_ASSERTIONS` (hardened bounds on `operator[]`) and `_FORTIFY_SOURCE=3`;
  every test must pass under ASan+UBSan.

## Python rules (builder)

- black is mandatory. Python 3.9 is the floor, matching portage.
- Import portage read-only: public-ish APIs (`portage.dep`, `vardbapi`, `config`) only, never
  `_emerge`. If a needed API is private, wrap it in one place so drift breaks one function.
- Tests build throwaway systems with portage's `ResolverPlayground` rather than mocking dbapis.

## Conventions

- Naming: `PascalCase` types, `snake_case` functions and variables, both languages.
- Keep the tree flat: `src/`, `tests/`, `builder/egraph_build/`, `builder/tests/`, `docs/`.
- Every feature lands as stub, then tests, then implementation; one roadmap step per commit series.
- Any divergence from portage needs a test proving egraph is right, recorded in
  `docs/upstream-notes.md`.
- Performance claims use `perf stat -e instructions:u,cycles:u` with interleaved A/B runs;
  wall clock on this box varies ±25% (see `docs/findings.md`).
- Update `docs/roadmap.md` status when a step lands, and `docs/findings.md` with new measurements.
