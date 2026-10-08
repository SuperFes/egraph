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

from portage.exception import ParseError
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
    # (name, entries) per file under env/, named by package.env or not.
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


def _dict_source(path, recursive, expected, prefixes=False, var="USE"):
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
        entries.append(Entry(name, number, atom, var, tuple(kept)))
    complete = all(consumed[atom] == len(tokens) for atom, tokens in flat)
    # Specificity breaks ties by the order of a cp's keys, which is all that has to agree.
    if complete and _by_cp(_keys(entries), cps) == _by_cp(wanted, cps):
        return tuple(entries)
    return tuple(Entry(path, 0, atom, var, tokens) for atom, tokens in flat)


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


def _env_names(directory):
    """The files under env/, as package.env names them."""
    if not os.path.isdir(directory):
        return []
    return [os.path.relpath(path, directory) for path in _file_list(directory, True)]


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


def _make_conf(files, expand=None):
    """make.conf's values; with expand, its references to what make.defaults set expanded, as
    config.__init__ reads it."""
    values = {}
    expand = {} if expand is None else expand
    for name in files:
        values.update(getconfig(name, allow_sourcing=True, expand=expand) or {})
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
    named = {name for entry in package_env for name in entry.tokens}
    env_files = []
    for name in sorted(named | set(_env_names(os.path.join(user, "env")))):
        try:
            entries = _env_file(settings, name, use_expand, unprefixed)
        except (ParseError, OSError):
            # One nothing names yet only stands ready to be tried.
            if name in named:
                raise
            continue
        env_files.append((name, tuple(entries)))
    env_files = tuple(env_files)

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


# The incremental variables visibility is decided from.
ACCEPT = ("ACCEPT_KEYWORDS", "ACCEPT_LICENSE", "ACCEPT_PROPERTIES", "ACCEPT_RESTRICT")


class VisibilityNode(NamedTuple):
    """A profile node's sources of visibility: make.defaults' ACCEPT variables, then its
    package.mask, package.unmask, package.keywords, package.accept_keywords and (in the
    profile-license format) package.license."""

    path: str
    defaults: tuple = ()
    package_mask: tuple = ()
    package_unmask: tuple = ()
    package_keywords: tuple = ()
    package_accept_keywords: tuple = ()
    package_license: tuple = ()


class MaskRepository(NamedTuple):
    name: str
    masters: tuple = ()
    package_mask: tuple = ()
    package_unmask: tuple = ()


class VisibilityLedger(NamedTuple):
    """Every source of a version's visibility, entry by entry: the ACCEPT variables by layer,
    the license groups, and the package.* files of the repositories, the profile nodes and the
    user. A mask file's entry is its line's atom, `-` and all, with no tokens."""

    env_d: tuple = ()
    # make.globals, and portage's built-in defaults (no file) for what it leaves out.
    globals: tuple = ()
    profiles: tuple = ()
    repositories: tuple = ()
    # make.conf, then the `*/*` lines of package.license, package.properties and
    # package.accept_restrict, as config.__init__ folds them into the conf layer.
    conf: tuple = ()
    env: tuple = ()
    # One entry per group definition: the group as var, its members as tokens.
    license_groups: tuple = ()
    package_mask: tuple = ()
    package_unmask: tuple = ()
    package_keywords: tuple = ()
    package_accept_keywords: tuple = ()
    package_license: tuple = ()
    package_properties: tuple = ()
    package_accept_restrict: tuple = ()


def _atom_lines(path, recursive, expected):
    """A package.mask-style file's entries, an atom per line, held to portage's values."""
    expected = tuple(str(value) for value, _ in expected)
    entries = []
    at = 0
    for name, number, tokens in _lines(path, recursive):
        kept, at = _subsequence([" ".join(tokens)], expected, at)
        if kept:
            entries.append(Entry(name, number, kept[0], "", ()))
    if at == len(expected):
        return tuple(entries)
    return tuple(Entry(path, 0, atom, "", ()) for atom in expected)


def _by_cp_dict(flat):
    """grabdict_package's {atom: tokens} as {cp: {atom: tokens}}, as the managers keep it."""
    grouped = {}
    for atom, tokens in flat.items():
        grouped.setdefault(atom.cp, {})[atom] = tokens
    return grouped


def _package_dict(path, recursive, without_global=False, **options):
    """A package.* file's entries as grabdict_package reads it; without its `*/*` line when
    portage folds that elsewhere."""
    from portage.util import grabdict_package

    grabbed = grabdict_package(path, recursive=recursive, **options)
    if without_global:
        grabbed.pop("*/*", None)
    return _dict_source(path, recursive, _by_cp_dict(grabbed), var="")


def _user_package_dict(path, without_global=False):
    return _package_dict(
        path,
        True,
        without_global,
        allow_wildcard=True,
        allow_repo=True,
        verify_eapi=False,
        allow_build_id=True,
        allow_use=False,
    )


def _profile_package_dict(node, name):
    from portage.repository.config import allow_profile_repo_deps

    return _package_dict(
        os.path.join(node.location, name),
        node.portage1_directories,
        verify_eapi=True,
        eapi=node.eapi,
        eapi_default=None,
        allow_repo=allow_profile_repo_deps(node),
        allow_build_id=node.allow_build_id,
        allow_use=False,
    )


def _accept_assigned(files, values):
    return [
        Entry(*_assignment(files, var), "", var, tuple(values[var].split()))
        for var in ACCEPT
        if values.get(var) is not None
    ]


def _accept_held(entries, layer, path):
    """entries where they stack to the layer's ACCEPT variables, concatenated as stack_dicts
    and regenerate concatenate incrementals; else portage's values at line 0."""
    stacked = {}
    for entry in entries:
        stacked.setdefault(entry.var, []).extend(entry.tokens)
    wanted = {var: layer[var].split() for var in ACCEPT if layer.get(var) is not None}
    if {k: v for k, v in stacked.items() if v} == {
        k: v for k, v in wanted.items() if v
    }:
        return tuple(entries)
    return tuple(Entry(path, 0, "", var, tuple(v)) for var, v in wanted.items())


def _global_lines(path, var):
    """A user package.* file's `*/*` lines, which config.__init__ folds into var."""
    return [
        Entry(name, number, "*/*", var, tuple(tokens[1:]))
        for name, number, tokens in _lines(path, True)
        if tokens[0] == "*/*"
    ]


def _license_groups(locations):
    """Each license_groups line as LicenseManager reads them: the group, then its members."""
    entries = []
    for location in locations:
        for name, number, tokens in _lines(
            os.path.join(location, "license_groups"), False
        ):
            if len(tokens) > 1:
                entries.append(Entry(name, number, "", tokens[0], tuple(tokens[1:])))
    return tuple(entries)


def _mask_file(path, recursive, **options):
    from portage.util import grabfile_package

    return _atom_lines(
        path,
        recursive,
        grabfile_package(
            path, recursive=recursive, remember_source_file=True, **options
        ),
    )


def _make_globals(settings):
    import portage
    from portage.const import PORTAGE_BASE_PATH

    if portage._not_installed:
        return os.path.join(PORTAGE_BASE_PATH, "cnf", "make.globals")
    return os.path.join(settings.global_config_path, "make.globals")


def read_visibility(settings):
    """The VisibilityLedger of settings, a config with nothing set."""
    from portage.repository.config import allow_profile_repo_deps

    locations = settings._locations_manager
    nodes = locations.profiles_complex
    profiles = []
    # make.defaults expanded node by node, then make.conf, as config.__init__ reads them.
    expand = dict(settings.configdict["env.d"])
    for node in nodes:
        path = os.path.join(node.location, "make.defaults")
        files = _file_list(path, node.portage1_directories)
        expand.pop("USE", None)
        values = (
            getconfig(path, expand=expand, recursive=node.portage1_directories) or {}
        )
        mask_options = dict(
            verify_eapi=True,
            eapi=node.eapi,
            eapi_default=None,
            allow_repo=allow_profile_repo_deps(node),
            allow_build_id=node.allow_build_id,
        )
        profiles.append(
            VisibilityNode(
                path=node.location,
                defaults=tuple(_accept_assigned(files, values)),
                package_mask=_mask_file(
                    os.path.join(node.location, "package.mask"),
                    node.portage1_directories,
                    **mask_options,
                ),
                package_unmask=(
                    _mask_file(
                        os.path.join(node.location, "package.unmask"),
                        node.portage1_directories,
                        **mask_options,
                    )
                    if node.portage1_directories
                    else ()
                ),
                package_keywords=_profile_package_dict(node, "package.keywords"),
                package_accept_keywords=_profile_package_dict(
                    node, "package.accept_keywords"
                ),
                package_license=(
                    _user_package_dict(
                        os.path.join(node.location, "package.license"), True
                    )
                    if "profile-license" in node.profile_formats
                    else ()
                ),
            )
        )
    defaults = tuple(entry for node in profiles for entry in node.defaults)
    held = _accept_held(defaults, settings.configdict["defaults"], "")
    if held != defaults and profiles:
        path = os.path.join(profiles[-1].path, "make.defaults")
        profiles = [node._replace(defaults=()) for node in profiles]
        profiles[-1] = profiles[-1]._replace(
            defaults=tuple(entry._replace(file=path) for entry in held)
        )

    repositories = []
    for repo in settings.repositories.repos_with_profiles():
        base = os.path.join(repo.location, "profiles")
        options = dict(
            verify_eapi=True,
            eapi_default=repo.eapi,
            allow_repo=allow_profile_repo_deps(repo),
            allow_build_id=("build-id" in repo.profile_formats),
        )
        repositories.append(
            MaskRepository(
                name=repo.name,
                masters=tuple(master.name for master in repo.masters),
                package_mask=_mask_file(
                    os.path.join(base, "package.mask"),
                    repo.portage1_profiles,
                    **options,
                ),
                package_unmask=(
                    _mask_file(os.path.join(base, "package.unmask"), True, **options)
                    if repo.portage1_profiles
                    else ()
                ),
            )
        )

    user = _user_config(settings)
    conf_files = _make_conf_files(settings)
    conf = _accept_assigned(conf_files, _make_conf(conf_files, expand))
    for name, var in (
        ("package.license", "ACCEPT_LICENSE"),
        ("package.properties", "ACCEPT_PROPERTIES"),
        ("package.accept_restrict", "ACCEPT_RESTRICT"),
    ):
        conf.extend(_global_lines(os.path.join(user, name), var))
    # package.license's `*/*` folds in with its groups expanded.
    licenses = settings._license_manager
    expanded = tuple(
        (
            entry._replace(tokens=tuple(licenses.expandLicenseTokens(entry.tokens)))
            if entry.atom == "*/*" and entry.var == "ACCEPT_LICENSE"
            else entry
        )
        for entry in conf
    )
    held = _accept_held(
        expanded, settings.configdict["conf"], conf_files[-1] if conf_files else ""
    )
    conf = tuple(conf) if held == expanded else held

    profile_env = os.path.join(settings["EROOT"], "etc", "profile.env")
    env_d = _accept_assigned([profile_env], settings.configdict["env.d"])
    made_globals = _make_globals(settings)
    return VisibilityLedger(
        env_d=_accept_held(env_d, settings.configdict["env.d"], profile_env),
        globals=_accept_held(
            tuple(
                entry if entry.line else entry._replace(file="")
                for entry in _accept_assigned(
                    [made_globals], settings.configdict["globals"]
                )
            ),
            settings.configdict["globals"],
            made_globals,
        ),
        profiles=tuple(profiles),
        repositories=tuple(repositories),
        conf=conf,
        env=tuple(
            Entry("", 0, "", var, tuple(settings.backupenv[var].split()))
            for var in ACCEPT
            if settings.backupenv.get(var) is not None
        ),
        license_groups=_license_groups(
            [*locations.profile_locations, locations.abs_user_config]
        ),
        package_mask=_mask_file(
            os.path.join(user, "package.mask"),
            True,
            allow_wildcard=True,
            allow_repo=True,
            verify_eapi=False,
            allow_build_id=True,
        ),
        package_unmask=_mask_file(
            os.path.join(user, "package.unmask"),
            True,
            allow_wildcard=True,
            allow_repo=True,
            verify_eapi=False,
            allow_build_id=True,
        ),
        package_keywords=_user_package_dict(os.path.join(user, "package.keywords")),
        package_accept_keywords=_user_package_dict(
            os.path.join(user, "package.accept_keywords")
        ),
        package_license=_user_package_dict(os.path.join(user, "package.license"), True),
        package_properties=_user_package_dict(
            os.path.join(user, "package.properties"), True
        ),
        package_accept_restrict=_user_package_dict(
            os.path.join(user, "package.accept_restrict"), True
        ),
    )


def visibility_to_json(vis):
    """The visibility ledger as a JSON value, for the repository index's canonical JSON."""
    return {
        "env_d": _entries_json(vis.env_d),
        "globals": _entries_json(vis.globals),
        "profiles": [
            {
                "path": node.path,
                **{
                    name: _entries_json(getattr(node, name))
                    for name in VisibilityNode._fields[1:]
                },
            }
            for node in vis.profiles
        ],
        "repositories": [
            {
                "name": repo.name,
                "masters": list(repo.masters),
                "package_mask": _entries_json(repo.package_mask),
                "package_unmask": _entries_json(repo.package_unmask),
            }
            for repo in vis.repositories
        ],
        **{
            name: _entries_json(getattr(vis, name))
            for name in VisibilityLedger._fields[4:]
        },
    }
