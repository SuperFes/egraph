# egraph

A persistent dependency graph of a Gentoo system, queryable in milliseconds.

Portage rebuilds its view of the installed system from `/var/db/pkg` on every run and keeps no
reverse index, so questions like "what depends on this?" or "why is this installed?" cost seconds.
egraph evaluates the installed packages once, stores the result as a single file, keeps it fresh
by checking the package database on every load, and answers from the stored graph.

```sh
egraph                               # the interactive app on a terminal, a line shell off one
egraph rdeps dev-libs/openssl        # what depends on it, and through which atom
egraph why net-misc/networkmanager   # path from @world / @system to the package
egraph soname libssl.so.3            # installed consumers of a soname
egraph broken                        # installed deps nothing installed satisfies
egraph orphans                       # what depclean would remove
egraph updates -DN --tree            # what emerge -uDN would merge, under what keeps it
egraph export --dot app-misc/foo     # neighborhood as graphviz
egraph rebuild | egraph check        # rebuild the store, or diff it against a fresh build
```

Every answer is held to portage's own on generated systems and on a live one. See
`docs/roadmap.md` for what is done and what comes next.

## Building

```sh
meson setup build && meson compile -C build && meson test -C build
meson install -C build
```

It needs a C++23 compiler, meson, CLI11, nlohmann_json, Catch2, Python 3.9 or later and portage;
Notcurses for the interactive app (`-Dtui`). `-Dportage_hooks` (the default) installs the portage
hooks that keep the system store current: syncs refresh it at once, and emerges do once
portage's post_emerge hook runs the dispatcher (without a post_emerge of your own):

```sh
ln -s /usr/share/egraph/post_emerge /etc/portage/bin/post_emerge
```

## Layout

- `src/`: the `egraph` binary (C++23): store reader, freshness check, queries, CLI.
- `builder/`: `egraph-build` (Python): evaluates packages through portage and writes the store.
- `docs/`: design, store format, roadmap, measured findings, notes for upstream portage.

## License

GPL-2 (`COPYING`), matching portage, which the builder imports.
