#include "use_stack.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <utility>

namespace egraph {

namespace {

constexpr std::array<std::pair<std::string_view, UseLayer>, 11> layer_names{{
    {"env.d", UseLayer::env_d},
    {"repo", UseLayer::repo},
    {"features", UseLayer::features},
    {"pkginternal", UseLayer::pkginternal},
    {"defaults", UseLayer::defaults},
    {"conf", UseLayer::conf},
    {"pkg", UseLayer::pkg},
    {"env", UseLayer::env},
    {"force", UseLayer::force},
    {"arch", UseLayer::arch},
    {"mask", UseLayer::mask},
}};

// The index of each of ledger_files.
constexpr std::size_t make_defaults = 0;
constexpr std::size_t use_stable = 1;
constexpr std::size_t use_force = 2;
constexpr std::size_t use_stable_force = 3;
constexpr std::size_t use_mask = 4;
constexpr std::size_t use_stable_mask = 5;
constexpr std::size_t package_use = 6;
constexpr std::size_t package_use_stable = 7;
constexpr std::size_t package_use_force = 8;
constexpr std::size_t package_use_stable_force = 9;
constexpr std::size_t package_use_mask = 10;
constexpr std::size_t package_use_stable_mask = 11;

struct Token {
    std::string_view text;
    std::optional<std::uint32_t> entry;
};

// One layer as regenerate reads it: USE, and the USE_EXPAND and USE_EXPAND_UNPREFIXED variables
// it sets.
struct Layer {
    std::vector<Token> use;
    std::map<std::string_view, std::vector<Token>, std::less<>> vars;
};

// The flags stacked so far, each step that touched one recorded.
class Flags {
  public:
    explicit Flags(StackedUse& out) : out_(&out) {}

    // named: the token names the flag itself, which counts even where it changes nothing.
    void set(const std::string& flag, bool enable, UseLayer layer, const Token& token, bool named) {
        const bool was = enabled_.contains(flag);
        if (enable) {
            enabled_.insert(flag);
        } else {
            enabled_.erase(flag);
        }
        if (named || was != enable) {
            out_->steps[flag].push_back({.layer = layer,
                                         .entry = token.entry,
                                         .token = std::string{token.text},
                                         .enabled = enable,
                                         .changed = was != enable});
        }
    }

    // Every enabled flag starting with prefix, or all of them when it is empty.
    void clear(std::string_view prefix, UseLayer layer, const Token& token) {
        std::vector<std::string> gone;
        for (const auto& flag : enabled_) {
            if (flag.starts_with(prefix)) {
                gone.push_back(flag);
            }
        }
        for (const auto& flag : gone) {
            set(flag, false, layer, token, false);
        }
    }

    [[nodiscard]] const std::set<std::string, std::less<>>& enabled() const { return enabled_; }

  private:
    StackedUse* out_;
    std::set<std::string, std::less<>> enabled_;
};

std::string lowered(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

// stack_lists(incremental=True) of force or mask: each flag in it with the token that left it
// in, and each taken out with the "-flag" that did.
struct Stacked {
    std::map<std::string, Token, std::less<>> in;
    std::map<std::string, Token, std::less<>> out;
};

Stacked stack_lists(const std::vector<Token>& tokens) {
    Stacked stacked;
    for (const auto& token : tokens) {
        if (token.text == "-*") {
            stacked.in.clear();
        } else if (token.text.starts_with('-')) {
            const auto flag = token.text.substr(1);
            if (const auto found = stacked.in.find(flag); found != stacked.in.end()) {
                stacked.in.erase(found);
                stacked.out.insert_or_assign(std::string{flag}, token);
            }
        } else {
            stacked.in.insert_or_assign(std::string{token.text}, token);
            if (const auto found = stacked.out.find(token.text); found != stacked.out.end()) {
                stacked.out.erase(found);
            }
        }
    }
    return stacked;
}

} // namespace

std::string_view layer_name(UseLayer layer) {
    for (const auto& [name, value] : layer_names) {
        if (value == layer) {
            return name;
        }
    }
    return "";
}

bool in_iuse(const Store& installed, const Evaluated& evaluated, const Candidate& candidate,
             std::string_view flag) {
    if (std::ranges::any_of(evaluated.ids_in(candidate.iuse),
                            [&](std::uint32_t id) { return evaluated.string(id) == flag; })) {
        return true;
    }
    const auto& implicit = installed.implicit;
    if (candidate.iuse_effective) {
        return std::ranges::contains(implicit.effective, flag);
    }
    return std::ranges::contains(implicit.literals, flag) ||
           std::ranges::any_of(implicit.prefixes, [flag](const std::string& prefix) {
               return flag.starts_with(prefix);
           });
}

UseStacker::UseStacker(const Store& installed, const Evaluated& evaluated)
    : installed_(&installed), evaluated_(&evaluated) {
    atoms_.reserve(evaluated.ledger_entries.size());
    for (const auto& entry : evaluated.ledger_entries) {
        std::optional<Atom> parsed;
        if (const auto text = evaluated.string(entry.atom); !text.empty()) {
            if (auto atom = parse_config_atom(text)) {
                parsed = std::move(*atom);
            }
        }
        atoms_.push_back(std::move(parsed));
    }
    for (std::uint32_t i = 0; i < atoms_.size(); ++i) {
        if (const auto& atom = atoms_.at(i); atom && atom->extended) {
            extended_.push_back(i);
        } else if (atom) {
            by_cp_[atom->cp].push_back(i);
        }
    }
}

StackedUse UseStacker::stack(const Candidate& candidate,
                             std::optional<UseStacker::Omitted> without) const {
    const auto& ev = *evaluated_;
    const auto& ledger = ev.ledger;
    const auto cp = ev.string(candidate.cp);
    const auto cpv = ev.string(candidate.cpv);
    const auto version =
        parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1))).value_or(Version{});
    const auto slot = ev.string(candidate.slot);
    const auto sub_slot = ev.string(candidate.sub_slot);
    const auto repo = ev.string(candidate.repo);
    const bool stable = candidate.stable;

    const auto left_out = [&](std::uint32_t index, std::uint32_t position) {
        return without && without->entry == index && without->position == position;
    };
    const auto tokens_of = [&](std::uint32_t index, std::vector<Token>& into) {
        const auto& entry = ev.ledger_entries.at(index);
        std::uint32_t position = 0;
        for (const auto id : ev.ids_in(entry.tokens)) {
            if (!left_out(index, position++)) {
                into.push_back({.text = ev.string(id), .entry = index});
            }
        }
    };
    const auto all_of = [&](Range source, std::vector<Token>& into) {
        for (std::uint32_t i = 0; i < source.count; ++i) {
            tokens_of(source.first + i, into);
        }
    };
    // A package.* source's tokens for the candidate, as ordered_by_atom_specificity applies
    // them: its keys (one per atom, its lines in order) as portage's dictionaries hold them for
    // the cp, plain atoms first, then the extended ones by their cp; least specific first.
    // Only entries of the candidate's cp, and extended ones, can match it.
    const auto* of_cp = [&]() -> const std::vector<std::uint32_t>* {
        const auto found = by_cp_.find(cp);
        return found == by_cp_.end() ? nullptr : &found->second;
    }();
    const auto matched = [&](Range source, std::vector<Token>& into) {
        std::vector<std::uint32_t> candidates;
        const auto within = [&](const std::vector<std::uint32_t>& indices) {
            const auto first = std::ranges::lower_bound(indices, source.first);
            const auto last = std::ranges::lower_bound(indices, source.first + source.count);
            candidates.insert(candidates.end(), first, last);
        };
        if (of_cp != nullptr) {
            within(*of_cp);
        }
        within(extended_);
        std::ranges::sort(candidates);
        std::vector<std::pair<std::string_view, std::vector<std::uint32_t>>> keys;
        for (const auto index : candidates) {
            const auto atom = ev.string(ev.ledger_entries.at(index).atom);
            const auto key = std::ranges::find(keys, atom, &decltype(keys)::value_type::first);
            if (key == keys.end()) {
                keys.push_back({atom, {index}});
            } else {
                key->second.push_back(index);
            }
        }
        std::vector<const std::vector<std::uint32_t>*> ordered;
        std::vector<Atom> atoms;
        const auto take = [&](const auto& key) {
            ordered.push_back(&key.second);
            atoms.push_back(*atoms_.at(key.second.front()));
        };
        for (const auto& key : keys) {
            const auto& atom = atoms_.at(key.second.front());
            if (atom && !atom->extended) {
                take(key);
            }
        }
        std::vector<std::string_view> extended_cps;
        for (const auto& key : keys) {
            const auto& atom = atoms_.at(key.second.front());
            if (atom && atom->extended &&
                std::ranges::find(extended_cps, atom->cp) == extended_cps.end()) {
                extended_cps.push_back(atom->cp);
            }
        }
        for (const auto extended : extended_cps) {
            for (const auto& key : keys) {
                const auto& atom = atoms_.at(key.second.front());
                if (atom && atom->extended && atom->cp == extended) {
                    take(key);
                }
            }
        }
        for (const auto index : by_specificity(atoms, cp, version, slot, sub_slot, repo)) {
            for (const auto entry : *ordered.at(index)) {
                tokens_of(entry, into);
            }
        }
    };
    // An entry into a layer: USE is incremental, every other variable replaced.
    const auto add = [&](Layer& layer, std::uint32_t index) {
        const auto& entry = ev.ledger_entries.at(index);
        const auto var = ev.string(entry.var);
        if (var == "USE") {
            tokens_of(index, layer.use);
        } else {
            auto& value = layer.vars[var];
            value.clear();
            tokens_of(index, value);
        }
    };
    const auto add_all = [&](Layer& layer, Range source) {
        for (std::uint32_t i = 0; i < source.count; ++i) {
            add(layer, source.first + i);
        }
    };
    const auto strings = [&](Range range) {
        std::vector<std::string_view> out;
        for (const auto id : ev.ids_in(range)) {
            out.push_back(ev.string(id));
        }
        return out;
    };
    const auto per_package = [&](Range range) {
        Layer layer;
        for (const auto id : ev.ids_in(range)) {
            layer.use.push_back({.text = ev.string(id), .entry = std::nullopt});
        }
        return layer;
    };

    // The repositories the candidate's inherits from, then its own.
    std::vector<const LedgerRepository*> repos;
    const auto find_repo = [&](std::string_view name) -> const LedgerRepository* {
        const auto found = std::ranges::find_if(
            ledger.repositories, [&](const auto& r) { return ev.string(r.name) == name; });
        return found == ledger.repositories.end() ? nullptr : &*found;
    };
    if (const auto* own = find_repo(repo)) {
        for (const auto master : strings(own->masters)) {
            if (const auto* found = find_repo(master)) {
                repos.push_back(found);
            }
        }
        repos.push_back(own);
    }

    std::map<UseLayer, Layer> layers;
    layers[UseLayer::pkginternal] = per_package(candidate.internal);
    layers[UseLayer::features] = per_package(candidate.features);

    // package.use.stable > package.use > use.stable, in each repository and profile node.
    const auto pick = [&](const LedgerSources& sources, std::vector<Token>& into) {
        if (stable) {
            all_of(sources.at(use_stable), into);
        }
        matched(sources.at(package_use), into);
        if (stable) {
            matched(sources.at(package_use_stable), into);
        }
    };
    auto& repo_layer = layers[UseLayer::repo];
    for (const auto* r : repos) {
        add_all(repo_layer, r->sources.at(make_defaults));
        pick(r->sources, repo_layer.use);
    }
    auto& defaults = layers[UseLayer::defaults];
    const auto unprefixed = strings(ledger.use_expand_unprefixed);
    for (const auto& node : ledger.profiles) {
        const auto source = node.sources.at(make_defaults);
        all_of(source, defaults.use);
        // Its USE_EXPAND_UNPREFIXED variables, as the profiles stack them: the last one set.
        for (std::uint32_t i = 0; i < source.count; ++i) {
            const auto& entry = ev.ledger_entries.at(source.first + i);
            const auto var = ev.string(entry.var);
            if (var != "USE" && std::ranges::contains(unprefixed, var)) {
                auto& value = defaults.vars[var];
                value.clear();
                std::uint32_t position = 0;
                for (const auto id : ev.ids_in(entry.tokens)) {
                    if (!left_out(source.first + i, position++)) {
                        value.push_back({.text = ev.string(id), .entry = source.first + i});
                    }
                }
            }
        }
        pick(node.sources, defaults.use);
    }
    add_all(layers[UseLayer::conf], ledger.conf);
    auto& pkg = layers[UseLayer::pkg];
    std::vector<Token> env_names;
    matched(ledger.package_env, env_names);
    for (const auto& name : env_names) {
        for (const auto& file : ledger.env_files) {
            if (ev.string(file.name) == name.text) {
                add_all(pkg, file.entries);
            }
        }
    }
    matched(ledger.package_use, pkg.use);
    add_all(layers[UseLayer::env], ledger.env);
    add_all(layers[UseLayer::env_d], ledger.env_d);

    StackedUse out;
    Flags flags(out);
    const auto iuse = strings(candidate.iuse);
    const auto use_expand = strings(ledger.use_expand);
    std::vector<UseLayer> order;
    for (const auto name : strings(ledger.use_order)) {
        const auto found =
            std::ranges::find(layer_names, name, &decltype(layer_names)::value_type::first);
        if (found != layer_names.end() && found->second <= UseLayer::env) {
            order.push_back(found->second);
        }
    }
    std::ranges::reverse(order);
    for (const auto which : order) {
        const auto& layer = layers[which];
        for (const auto var : unprefixed) {
            const auto found = layer.vars.find(var);
            if (found == layer.vars.end()) {
                continue;
            }
            for (const auto& token : found->second) {
                const auto minus = token.text.starts_with('-');
                flags.set(std::string{token.text.substr(minus ? 1 : 0)}, !minus, which, token,
                          true);
            }
        }
        std::vector<std::string_view> expanded;
        for (const auto var : use_expand) {
            if (layer.vars.contains(var)) {
                expanded.push_back(var);
            }
        }
        if (layer.use.empty() && expanded.empty()) {
            continue;
        }
        for (const auto& token : layer.use) {
            auto text = token.text;
            if (text == "-*") {
                flags.clear("", which, token);
                continue;
            }
            if (text.starts_with('+')) {
                text.remove_prefix(1);
                if (text.empty()) {
                    continue;
                }
            }
            if (text.starts_with('-')) {
                if (text.ends_with("_*")) {
                    flags.clear(text.substr(1, text.size() - 2), which, token);
                }
                flags.set(std::string{text.substr(1)}, false, which, token, true);
                continue;
            }
            if (text.ends_with("_*")) {
                // Expanded against IUSE, so USE="linguas_* -linguas_en" works; passed through
                // where IUSE has none, as portage does.
                const auto prefix = text.substr(0, text.size() - 1);
                bool any = false;
                for (const auto flag : iuse) {
                    if (flag.starts_with(prefix)) {
                        any = true;
                        flags.set(std::string{flag}, true, which, token, false);
                    }
                }
                if (!any) {
                    flags.set(std::string{text}, true, which, token, true);
                }
                continue;
            }
            flags.set(std::string{text}, true, which, token, true);
        }
        // make.defaults' USE_EXPAND variables were expanded into its USE already.
        if (which == UseLayer::defaults) {
            continue;
        }
        for (const auto var : expanded) {
            const auto prefix = lowered(var) + "_";
            const auto& value = layer.vars.find(var)->second;
            const Token replaced{.text = var,
                                 .entry = value.empty() ? std::nullopt : value.front().entry};
            flags.clear(prefix, which, replaced);
            for (const auto& token : value) {
                // + and - are invalid in a non-incremental variable, which portage skips.
                if (token.text.starts_with('+') || token.text.starts_with('-')) {
                    continue;
                }
                flags.set(prefix + std::string{token.text}, true, which, token, true);
            }
        }
    }

    // useforce and usemask: each repository's files, then each profile node's, incrementally.
    std::vector<Token> force;
    std::vector<Token> mask;
    const auto masks = [&](const LedgerSources& sources) {
        all_of(sources.at(use_force), force);
        if (stable) {
            all_of(sources.at(use_stable_force), force);
        }
        matched(sources.at(package_use_force), force);
        if (stable) {
            matched(sources.at(package_use_stable_force), force);
        }
        all_of(sources.at(use_mask), mask);
        if (stable) {
            all_of(sources.at(use_stable_mask), mask);
        }
        matched(sources.at(package_use_mask), mask);
        if (stable) {
            matched(sources.at(package_use_stable_mask), mask);
        }
    };
    for (const auto* r : repos) {
        masks(r->sources);
    }
    for (const auto& node : ledger.profiles) {
        masks(node.sources);
    }
    const auto forced = stack_lists(force);
    const auto masked = stack_lists(mask);
    // A force or mask a later file took back changes nothing, but tells why there is none.
    for (const auto& [flag, token] : forced.out) {
        flags.set(flag, flags.enabled().contains(flag), UseLayer::force, token, true);
    }
    for (const auto& [flag, token] : forced.in) {
        flags.set(flag, true, UseLayer::force, token, true);
    }
    if (const auto arch = ev.string(ledger.arch); !arch.empty()) {
        flags.set(std::string{arch}, true, UseLayer::arch, {.text = arch, .entry = std::nullopt},
                  true);
    }
    for (const auto& [flag, token] : masked.out) {
        flags.set(flag, flags.enabled().contains(flag), UseLayer::mask, token, true);
    }
    for (const auto& [flag, token] : masked.in) {
        flags.set(flag, false, UseLayer::mask, token, true);
    }

    for (const auto& flag : flags.enabled()) {
        if (!flag.ends_with("_*") && in_iuse(*installed_, ev, candidate, flag)) {
            out.use.push_back(flag);
        }
    }
    for (const auto flag : iuse) {
        if (forced.in.contains(flag) || masked.in.contains(flag)) {
            out.forced.emplace_back(flag);
        }
    }
    std::ranges::sort(out.forced);
    return out;
}

} // namespace egraph
