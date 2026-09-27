# egraph

A persistent dependency graph of a Gentoo system, queryable in milliseconds.

Portage rebuilds its view of the installed system from `/var/db/pkg` on every run and keeps no
reverse index, so questions like "what depends on this?" or "why is this installed?" cost seconds.
egraph evaluates the installed packages once, stores the result as a single file, keeps it fresh
by checking the package database on every load, and answers from the stored graph.

```sh
egraph rdeps dev-libs/openssl        # what depends on it, and through which atom
egraph why net-misc/networkmanager   # path from @world / @system to the package
egraph soname libssl.so.3            # installed consumers of a soname
egraph broken                        # installed deps nothing installed satisfies
egraph export --dot app-misc/foo     # neighborhood as graphviz
egraph rebuild | egraph check        # rebuild the store, or diff it against a fresh build
```

Status: scaffold; every command is a stub. See `docs/roadmap.md` and `TODO.md`.

## Layout

- `src/`: the `egraph` binary (C++23): store reader, freshness check, queries, CLI.
- `builder/`: `egraph-build` (Python): evaluates packages through portage and writes the store.
- `docs/`: design, store format, roadmap, measured findings, notes for upstream portage.

## License

GPL-2, matching portage, which the builder imports.
