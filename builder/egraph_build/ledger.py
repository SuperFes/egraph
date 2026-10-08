"""Every source of a flag's state, entry by entry with the file and line it was read from, as
portage stacks a package's USE (config.setcpv and regenerate, UseManager).

The values are portage's own: each source is read line by line for where its entries are, then
held to what portage loaded. A source that differs is kept as portage holds it, at line 0.

Portage keeps these behind private config attributes; this is the one place that reads them,
so drift in portage breaks this module and nothing else.
"""

import os
import re
from typing import NamedTuple

from portage.util import getconfig

# The twelve files of a profile node or a repository's profiles directory, in the order the
# store keeps them, with the UseManager attributes holding them: per profile node, then per
# repository (None where a repository has no such list, as for make.defaults).
KINDS = (
    ("make.defaults", None, None),
    ("use.stable", "_use_stable_list", "_repo_use_stable_dict"),
    ("use.force", "_useforce_list", "_repo_useforce_dict"),
    ("use.stable.force", "_usestableforce_list", "_repo_usestableforce_dict"),
    ("use.mask", "_usemask_list", "_repo_usemask_dict"),
    ("use.stable.mask", "_usestablemask_list", "_repo_usestablemask_dict"),
    ("package.use", "_pkgprofileuse", "_repo_puse_dict"),
    ("package.use.stable", "_puse_stable_list", "_repo_puse_stable_dict"),
    ("package.use.force", "_puseforce_list", "_repo_puseforce_dict"),
    ("package.use.stable.force", "_pusestableforce_list", "_repo_pusestableforce_dict"),
    ("package.use.mask", "_pusemask_list", "_repo_pusemask_dict"),
    ("package.use.stable.mask", "_pusestablemask_list", "_repo_pusestablemask_dict"),
)
FILES = tuple(name for name, _, _ in KINDS)


class Entry(NamedTuple):
    file: str
    # 0 where portage's value could not be told apart line by line.
    line: int
    # Empty for a global entry.
    atom: str
    # USE, or the USE_EXPAND or USE_EXPAND_UNPREFIXED variable it was set through.
    var: str
    tokens: tuple


class Node(NamedTuple):
    path: str
    # One entry tuple per FILES.
    sources: tuple


class Repository(NamedTuple):
    name: str
    masters: tuple
    sources: tuple


class Ledger(NamedTuple):
    use_order: tuple = ()
    use_expand: tuple = ()
    use_expand_unprefixed: tuple = ()
    arch: str = ""
    profiles: tuple = ()
    repositories: tuple = ()
    conf: tuple = ()
    package_use: tuple = ()
    package_env: tuple = ()
    # (name, entries) per file under env/ that package.env names.
    env_files: tuple = ()
    env: tuple = ()
    env_d: tuple = ()
    features: tuple = ()


def _file_list(path, recursive):
    from portage.util import _recursive_file_list

    if recursive:
        return list(_recursive_file_list(path))
    return [path] if os.path.isfile(path) else []


def _lines(path, recursive):
    """(file, line number, tokens) for every line grabdict reads, comments cut off."""
    for name in _file_list(path, recursive):
        try:
            with open(name, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except OSError:
            continue
        for number, text in enumerate(lines, 1):
            if text[:1] == "#":
                continue
            tokens = []
            for token in text.split():
                if token[:1] == "#":
                    break
                tokens.append(token)
            if tokens:
                yield name, number, tokens


def _subsequence(tokens, expected, start):
    """(the tokens matched in order against expected from start, where matching stopped)."""
    kept = []
    for token in tokens:
        if start < len(expected) and token == expected[start]:
            kept.append(token)
            start += 1
    return kept, start


def _tuple_source(path, recursive, expected):
    """A use.* file's entries, one flag per line as grabfile reads them."""
    expected = tuple(expected)
    entries = []
    at = 0
    for name, number, tokens in _lines(path, recursive):
        kept, at = _subsequence([" ".join(tokens)], expected, at)
        if kept:
            entries.append(Entry(name, number, "", "USE", tuple(kept)))
    if at == len(expected):
        return tuple(entries)
    return (Entry(path, 0, "", "USE", expected),) if expected else ()


def _flatten(expected):
    """portage's {cp: {atom: tokens}} (or an ExtendedAtomDict of them) as [(atom, tokens)] in
    its order, cp by cp."""
    flat = []
    for atoms in expected.values():
        for atom, tokens in atoms.items():
            if isinstance(tokens, str):
                tokens = tokens.split()
            flat.append((str(atom), tuple(tokens)))
    return flat


def _expand_prefixes(tokens):
    """A user package.use line's flags with `VAR:` prefixes put on, as UseManager does."""
    out = []
    prefix = ""
    for token in tokens:
        if token[-1:] == ":":
            prefix = token[:-1].lower() + "_"
        elif token[:1] == "-":
            out.append("-" + prefix + token[1:])
        else:
            out.append(prefix + token)
    return out


def _dict_source(path, recursive, expected, prefixes=False):
    """A package.* file's entries, one per line, each its atom and the tokens portage kept."""
    flat = _flatten(expected)
    cps = {str(atom): atom.cp for atoms in expected.values() for atom in atoms}
    wanted = dict(flat)
    consumed = dict.fromkeys(wanted, 0)
    entries = []
    for name, number, tokens in _lines(path, recursive):
        atom = tokens[0]
        if atom not in wanted:
            continue
        values = _expand_prefixes(tokens[1:]) if prefixes else tokens[1:]
        kept, consumed[atom] = _subsequence(values, wanted[atom], consumed[atom])
        entries.append(Entry(name, number, atom, "USE", tuple(kept)))
    complete = all(consumed[atom] == len(tokens) for atom, tokens in flat)
    # Specificity breaks ties by the order of a cp's keys, which is all that has to agree.
    if complete and _by_cp(_keys(entries), cps) == _by_cp(wanted, cps):
        return tuple(entries)
    return tuple(Entry(path, 0, atom, "USE", tokens) for atom, tokens in flat)


def _keys(entries):
    return list(dict.fromkeys(entry.atom for entry in entries))


def _by_cp(atoms, cps):
    grouped = {}
    for atom in atoms:
        grouped.setdefault(cps[atom], []).append(atom)
    return grouped


def _assignment(files, var):
    """(file, line) of the last assignment to var among files, or (the last file, 0)."""
    pattern = re.compile(rf"^\s*(export\s+)?{re.escape(var)}=")
    found = (files[-1] if files else "", 0)
    for name in files:
        try:
            with open(name, encoding="utf-8", errors="replace") as f:
                for number, text in enumerate(f, 1):
                    if pattern.match(text):
                        found = (name, number)
        except OSError:
            continue
    return found


def _variables(values, use_expand, unprefixed):
    """USE and then the USE_EXPAND variables values sets, in that order."""
    return [
        var for var in ("USE", *unprefixed, *use_expand) if values.get(var) is not None
    ]


def _assigned(files, values, use_expand, unprefixed):
    return [
        Entry(*_assignment(files, var), "", var, tuple(values[var].split()))
        for var in _variables(values, use_expand, unprefixed)
    ]


def _make_defaults(settings, node, use_expand, unprefixed, expand, expected):
    """A profile node's make.defaults entries, expanded as regenerate puts them in
    make_defaults_use; portage's string at line 0 where they differ."""
    path = os.path.join(node.location, "make.defaults")
    expand.pop("USE", None)
    values = getconfig(path, expand=expand, recursive=node.portage1_directories) or {}
    files = _file_list(path, node.portage1_directories)
    set_anywhere = [var for var in use_expand if settings.get(var) is not None]
    entries = []
    for var in unprefixed:
        if values.get(var) is not None:
            entries.append(
                Entry(*_assignment(files, var), "", var, tuple(values[var].split()))
            )
    for var in set_anywhere:
        if values.get(var) is None:
            continue
        prefix = var.lower() + "_"
        tokens = tuple(
            "-" + prefix + x[1:] if x[:1] == "-" else prefix + x
            for x in values[var].split()
        )
        entries.append(Entry(*_assignment(files, var), "", var, tokens))
    if values.get("USE") is not None:
        entries.append(
            Entry(*_assignment(files, "USE"), "", "USE", tuple(values["USE"].split()))
        )
    if [t for e in entries for t in e.tokens] == expected.split():
        return tuple(entries)
    return (Entry(path, 0, "", "USE", tuple(expected.split())),) if expected else ()


def _stacked(entries):
    """{var: value} as a layer stacks its entries: USE, incremental, concatenated; the
    USE_EXPAND variables, which INCREMENTALS never holds, replaced."""
    values = {}
    for entry in entries:
        if entry.var in values and entry.var == "USE":
            values[entry.var] = values[entry.var] + list(entry.tokens)
        else:
            values[entry.var] = list(entry.tokens)
    return values


def _held(entries, layer, use_expand, unprefixed, path):
    """entries where they stack to the layer's values, else portage's values at line 0."""
    # An empty USE stacks as none; an empty USE_EXPAND variable still clears its prefix.
    stacked = {k: v for k, v in _stacked(entries).items() if k != "USE" or v}
    wanted = {
        var: layer[var].split() for var in _variables(layer, use_expand, unprefixed)
    }
    wanted = {k: v for k, v in wanted.items() if k != "USE" or v}
    if stacked == wanted:
        return tuple(entries)
    return tuple(Entry(path, 0, "", var, tuple(v)) for var, v in wanted.items())


def _global_package_use(path):
    """The user package.use's `*/*` lines, which extract_global_USE_changes moves to the conf
    layer."""
    return [
        Entry(name, number, "*/*", "USE", tuple(_expand_prefixes(tokens[1:])))
        for name, number, tokens in _lines(path, True)
        if tokens[0] == "*/*"
    ]


def _env_file(settings, name, use_expand, unprefixed):
    path = os.path.join(_user_config(settings), "env", name)
    values = (
        getconfig(path, allow_sourcing=True, expand=settings._expand_map.copy()) or {}
    )
    return _assigned([path], values, use_expand, unprefixed)


def _user_config(settings):
    from portage.const import USER_CONFIG_PATH

    return os.path.join(settings["PORTAGE_CONFIGROOT"], USER_CONFIG_PATH)


def _make_conf_files(settings):
    from portage.const import MAKE_CONF_FILE

    root = settings["PORTAGE_CONFIGROOT"]
    paths = [os.path.join(root, "etc", "make.conf"), os.path.join(root, MAKE_CONF_FILE)]
    try:
        if os.path.samefile(*paths):
            paths.pop()
    except OSError:
        pass
    files = []
    for path in paths:
        if os.path.exists(path):
            files.extend(_file_list(path, True))
    return files


def _make_conf(files):
    values = {}
    for name in files:
        values.update(getconfig(name, allow_sourcing=True, expand={}) or {})
    return values


def read(settings):
    """The Ledger of settings, a config with its global USE stacked (no package set)."""
    from portage.util import grabdict_package

    manager = settings._use_manager
    use_expand = tuple(settings.get("USE_EXPAND", "").split())
    unprefixed = tuple(settings.get("USE_EXPAND_UNPREFIXED", "").split())

    profiles = []
    expand = {}
    nodes = settings._locations_manager.profiles_complex
    for i, node in enumerate(nodes):
        sources = [
            _make_defaults(
                settings,
                node,
                use_expand,
                unprefixed,
                expand,
                settings.make_defaults_use[i],
            )
        ]
        for name, attribute, _ in KINDS[1:]:
            path = os.path.join(node.location, name)
            held = getattr(manager, attribute)[i]
            recursive = node.portage1_directories
            if name.startswith("package."):
                sources.append(
                    _dict_source(path, recursive, held, prefixes=node.user_config)
                )
            else:
                sources.append(_tuple_source(path, recursive, held))
        profiles.append(Node(node.location, tuple(sources)))

    repositories = []
    for repo in settings.repositories.repos_with_profiles():
        base = os.path.join(repo.location, "profiles")
        made = settings._repo_make_defaults.get(repo.name, {})
        files = _file_list(os.path.join(base, "make.defaults"), repo.portage1_profiles)
        sources = [tuple(_assigned(files, made, use_expand, unprefixed))]
        for name, _, attribute in KINDS[1:]:
            path = os.path.join(base, name)
            held = getattr(manager, attribute).get(repo.name, {})
            if name.startswith("package."):
                sources.append(_dict_source(path, False, held))
            else:
                sources.append(_tuple_source(path, False, held))
        repositories.append(
            Repository(
                repo.name,
                tuple(master.name for master in repo.masters),
                tuple(sources),
            )
        )

    user = _user_config(settings)
    penv_path = os.path.join(user, "package.env")
    everywhere = grabdict_package(
        penv_path,
        recursive=1,
        allow_wildcard=True,
        allow_repo=True,
        verify_eapi=False,
        allow_build_id=True,
    ).get("*/*", ())
    conf_files = _make_conf_files(settings)
    # make.conf, then the `*/*` lines of package.use and of package.env, as config.__init__
    # folds them into the conf layer.
    conf = _assigned(conf_files, _make_conf(conf_files), use_expand, unprefixed)
    conf.extend(_global_package_use(os.path.join(user, "package.use")))
    for name in everywhere:
        conf.extend(_env_file(settings, name, use_expand, unprefixed))
    conf = _held(
        conf,
        settings.configdict["conf"],
        use_expand,
        unprefixed,
        conf_files[-1] if conf_files else "",
    )

    package_env = _dict_source(penv_path, True, settings._penvdict)
    names = sorted({name for entry in package_env for name in entry.tokens})
    env_files = tuple(
        (name, tuple(_env_file(settings, name, use_expand, unprefixed)))
        for name in names
    )

    profile_env = os.path.join(settings["EROOT"], "etc", "profile.env")
    return Ledger(
        use_order=tuple(settings.get("USE_ORDER", "").split(":")),
        use_expand=use_expand,
        use_expand_unprefixed=unprefixed,
        arch=settings.configdict["defaults"].get("ARCH", ""),
        profiles=tuple(profiles),
        repositories=tuple(repositories),
        conf=conf,
        package_use=_dict_source(
            os.path.join(user, "package.use"), True, manager._pusedict, prefixes=True
        ),
        package_env=package_env,
        env_files=env_files,
        # The env layer as reset() restores it: regenerate writes the stacked USE over it.
        env=tuple(
            Entry("", 0, "", var, tuple(settings.backupenv[var].split()))
            for var in _variables(settings.backupenv, use_expand, unprefixed)
        ),
        env_d=_held(
            _assigned(
                [profile_env], settings.configdict["env.d"], use_expand, unprefixed
            ),
            settings.configdict["env.d"],
            use_expand,
            unprefixed,
            profile_env,
        ),
        features=tuple(settings.configdict["features"].get("USE", "").split()),
    )


class PackageLayers(NamedTuple):
    # Its keywords accepted as stable ones, which brings in the *.stable* files.
    stable: bool = False
    # The pkginternal layer's USE: IUSE defaults, and -test where RESTRICT drops the feature.
    internal: tuple = ()
    # The features layer's USE: test under FEATURES=test.
    features: tuple = ()


def package_layers(settings):
    """The PackageLayers of the package settings was last setcpv'd to."""
    pkg = settings.mycpv
    return PackageLayers(
        stable=bool(settings._use_manager._isStable(pkg)),
        internal=tuple(settings.configdict["pkginternal"].get("USE", "").split()),
        features=tuple(settings.configdict["features"].get("USE", "").split()),
    )


def _entries_json(entries):
    return [
        {
            "file": e.file,
            "line": e.line,
            "atom": e.atom,
            "var": e.var,
            "tokens": list(e.tokens),
        }
        for e in entries
    ]


def _sources_json(sources):
    return {name: _entries_json(entries) for name, entries in zip(FILES, sources)}


def to_json(ledger):
    """The ledger as a JSON value, for the evaluated layer's canonical JSON."""
    return {
        "use_order": list(ledger.use_order),
        "use_expand": list(ledger.use_expand),
        "use_expand_unprefixed": list(ledger.use_expand_unprefixed),
        "arch": ledger.arch,
        "profiles": [
            {"path": node.path, **_sources_json(node.sources)}
            for node in ledger.profiles
        ],
        "repositories": [
            {
                "name": repo.name,
                "masters": list(repo.masters),
                **_sources_json(repo.sources),
            }
            for repo in ledger.repositories
        ],
        "conf": _entries_json(ledger.conf),
        "package_use": _entries_json(ledger.package_use),
        "package_env": _entries_json(ledger.package_env),
        "env_files": [
            {"name": name, "entries": _entries_json(entries)}
            for name, entries in ledger.env_files
        ],
        "env": _entries_json(ledger.env),
        "env_d": _entries_json(ledger.env_d),
        "features": list(ledger.features),
    }
