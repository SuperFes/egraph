#include "visibility_stack.hpp"

#include "atom.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <numeric>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace egraph {

namespace {

constexpr std::string_view accept_keywords_var = "ACCEPT_KEYWORDS";
constexpr std::string_view accept_license_var = "ACCEPT_LICENSE";
constexpr std::string_view accept_properties_var = "ACCEPT_PROPERTIES";
constexpr std::string_view accept_restrict_var = "ACCEPT_RESTRICT";

// The cp an atom is filed under: a wildcard atom's pattern, without its slot and repository.
std::string cp_of(std::string_view atom) {
    if (auto parsed = parse_atom(atom)) {
        return std::move(parsed->cp);
    }
    atom = atom.substr(0, atom.find("::"));
    return std::string{atom.substr(0, atom.find(':'))};
}

// The atom without its ::repo, as stack_lists' ignore_repo compares them.
std::string without_repo(std::string_view atom) {
    const auto at = atom.find("::");
    if (at == std::string_view::npos) {
        return std::string{atom};
    }
    const auto end = atom.find('[', at);
    return std::string{atom.substr(0, at)} +
           std::string{end == std::string_view::npos ? std::string_view{} : atom.substr(end)};
}

// Atom.with_repo, unless the atom names one already.
std::string with_repo(std::string_view atom, std::string_view repo) {
    if (atom.contains("::")) {
        return std::string{atom};
    }
    const auto use = std::min(atom.find('['), atom.size());
    return std::format("{}::{}{}", atom.substr(0, use), repo, atom.substr(use));
}

// stack_lists over atoms: a dict keyed by atom, in the order first added.
class AtomStack {
  public:
    // incremental: "-*" clears, "-atom" removes atom (with ignore_repo, any atom that is it
    // with a repository too, when it names none).
    void add(const StackedMask& mask, bool incremental, bool ignore_repo) {
        if (incremental && mask.atom == "-*") {
            items_.clear();
            return;
        }
        if (incremental && mask.atom.starts_with('-')) {
            const auto removed = std::string_view{mask.atom}.substr(1);
            if (ignore_repo && !removed.contains("::")) {
                std::erase_if(items_,
                              [&](const auto& item) { return without_repo(item.atom) == removed; });
            } else {
                std::erase_if(items_, [&](const auto& item) { return item.atom == removed; });
            }
            return;
        }
        const auto found =
            std::ranges::find_if(items_, [&](const auto& item) { return item.atom == mask.atom; });
        if (found == items_.end()) {
            items_.push_back(mask);
        } else {
            found->entry = mask.entry;
        }
    }

    void add(std::span<const StackedMask> masks, bool incremental, bool ignore_repo) {
        for (const auto& mask : masks) {
            add(mask, incremental, ignore_repo);
        }
    }

    [[nodiscard]] const std::vector<StackedMask>& items() const EGRAPH_LIFETIMEBOUND {
        return items_;
    }

  private:
    std::vector<StackedMask> items_;
};

// The keys of a manager's dict in its order: plain cps as first seen, then the wildcard ones.
template <class T, class AtomOf> std::vector<T> by_cp(std::vector<T> items, const AtomOf& atom_of) {
    std::vector<std::string> cps;
    std::map<std::string, std::vector<T>, std::less<>> grouped;
    for (auto& item : items) {
        auto cp = cp_of(atom_of(item));
        if (!grouped.contains(cp)) {
            cps.push_back(cp);
        }
        grouped[cp].push_back(std::move(item));
    }
    std::ranges::stable_partition(cps, [](const auto& cp) { return !cp.contains('*'); });
    std::vector<T> found;
    for (const auto& cp : cps) {
        for (auto& item : grouped.at(cp)) {
            found.push_back(std::move(item));
        }
    }
    return found;
}

// {atom: tokens} in the order first added.
class KeyDict {
  public:
    // Another line for key's atom within one source: its tokens join the key's.
    void extend(StackedKey key) {
        if (auto* found = find(key.atom)) {
            found->tokens.insert(found->tokens.end(), key.tokens.begin(), key.tokens.end());
            found->entries.insert(found->entries.end(), key.entries.begin(), key.entries.end());
        } else {
            keys_.push_back(std::move(key));
        }
    }

    // A later source's key replaces an earlier one's, where it was.
    void set(StackedKey key) {
        if (auto* found = find(key.atom)) {
            *found = std::move(key);
        } else {
            keys_.push_back(std::move(key));
        }
    }

    [[nodiscard]] std::vector<StackedKey>& keys() EGRAPH_LIFETIMEBOUND { return keys_; }

    [[nodiscard]] std::vector<StackedKey> ordered() && {
        return by_cp(std::move(keys_),
                     [](const StackedKey& key) -> std::string_view { return key.atom; });
    }

  private:
    StackedKey* find(std::string_view atom) {
        const auto found =
            std::ranges::find_if(keys_, [&](const auto& key) { return key.atom == atom; });
        return found == keys_.end() ? nullptr : &*found;
    }

    std::vector<StackedKey> keys_;
};

class Stacker {
  public:
    explicit Stacker(const RepositoryIndex& index) : index_{&index} {
        for (const auto id : ids(index.ledger.license_groups)) {
            const auto& entry = this->entry(id);
            auto& members = groups_[std::string{index.string(entry.var)}];
            for (const auto token : index.ids_in(entry.tokens)) {
                members.insert(std::string{index.string(token)});
            }
        }
    }

    [[nodiscard]] StackedVisibility stack() const {
        const auto& ledger = index_->ledger;
        StackedVisibility found;
        found.accept_keywords = incremental(accept_keywords_var);
        found.environment_keywords = tokens(ids(ledger.env), accept_keywords_var);
        for (const auto& node : ledger.profiles) {
            found.profile_keywords.push_back(source(node.package_keywords).ordered());
            found.profile_accept_keywords.push_back(source(node.package_accept_keywords).ordered());
        }
        found.accept_keywords_entries = user_keywords();
        found.masks = masks(false);
        found.unmasks = masks(true);
        auto accept_license = pruned(accept_license_var);
        if (accept_license.empty()) {
            accept_license = {{.token = "*", .entry = std::nullopt},
                              {.token = "-@EULA", .entry = std::nullopt}};
        }
        found.accept_license = expanded(accept_license);
        KeyDict licenses;
        auto license_sources = ledger.profiles |
                               std::views::transform(&VisibilityNode::package_license) |
                               std::ranges::to<std::vector>();
        license_sources.push_back(ledger.package_license);
        for (const auto range : license_sources) {
            auto keys = source(range);
            for (auto& key : keys.keys()) {
                key.tokens = expanded(key.tokens);
                licenses.set(std::move(key));
            }
        }
        found.licenses = std::move(licenses).ordered();
        found.accept_properties = pruned(accept_properties_var);
        found.properties = source(ledger.package_properties).ordered();
        found.accept_restrict = pruned(accept_restrict_var);
        found.restrict = source(ledger.package_accept_restrict).ordered();
        return found;
    }

  private:
    [[nodiscard]] const LedgerEntry& entry(std::uint32_t id) const {
        return index_->ledger_entries.at(id);
    }

    [[nodiscard]] static std::vector<std::uint32_t> ids(Range range) {
        std::vector<std::uint32_t> found(range.count);
        std::ranges::iota(found, range.first);
        return found;
    }

    // The entries of each layer regenerate stacks the ACCEPT_ variables over, in order:
    // env.d, make.globals, the profiles' make.defaults, make.conf (with the `*/*` lines folded
    // into it), the environment.
    [[nodiscard]] std::vector<std::uint32_t> accept_layers() const {
        const auto& ledger = index_->ledger;
        std::vector<Range> ranges{ledger.env_d, ledger.globals};
        for (const auto& node : ledger.profiles) {
            ranges.push_back(node.defaults);
        }
        ranges.push_back(ledger.conf);
        ranges.push_back(ledger.env);
        std::vector<std::uint32_t> found;
        for (const auto range : ranges) {
            const auto more = ids(range);
            found.insert(found.end(), more.begin(), more.end());
        }
        return found;
    }

    // The tokens var's entries among ids give it, in order.
    [[nodiscard]] std::vector<SourcedToken> tokens(std::span<const std::uint32_t> entries,
                                                   std::string_view var) const {
        std::vector<SourcedToken> found;
        for (const auto id : entries) {
            if (index_->string(entry(id).var) != var) {
                continue;
            }
            for (const auto token : index_->ids_in(entry(id).tokens)) {
                found.push_back({.token = std::string{index_->string(token)}, .entry = id});
            }
        }
        return found;
    }

    // regenerate's incremental stacking: -* clears, -x removes x, x adds it once.
    [[nodiscard]] std::vector<SourcedToken> incremental(std::string_view var) const {
        std::vector<SourcedToken> found;
        for (auto& token : tokens(accept_layers(), var)) {
            if (token.token == "-*") {
                found.clear();
            } else if (token.token.starts_with('-')) {
                const auto name = std::string_view{token.token}.substr(1);
                std::erase_if(found, [&](const auto& kept) { return kept.token == name; });
            } else if (auto kept = std::ranges::find(found, token.token, &SourcedToken::token);
                       kept != found.end()) {
                kept->entry = token.entry;
            } else {
                found.push_back(std::move(token));
            }
        }
        return found;
    }

    // prune_incremental of every layer's tokens: from the last * on, or after the last -*.
    [[nodiscard]] std::vector<SourcedToken> pruned(std::string_view var) const {
        auto found = tokens(accept_layers(), var);
        for (auto at = found.size(); at > 0; --at) {
            const auto& token = found.at(at - 1).token;
            if (token == "*") {
                found.erase(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(at - 1));
                break;
            }
            if (token == "-*") {
                found.erase(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            }
        }
        return found;
    }

    // LicenseManager.expandLicenseTokens: @group as its members, -@group as theirs negated; a
    // group met again within one token's expansion, or undefined, stays as it is.
    [[nodiscard]] std::vector<SourcedToken> expanded(std::span<const SourcedToken> tokens) const {
        std::vector<SourcedToken> found;
        for (const auto& token : tokens) {
            std::set<std::string, std::less<>> traversed;
            for (auto& name : expand(token.token, traversed)) {
                found.push_back({.token = std::move(name), .entry = token.entry});
            }
        }
        return found;
    }

    [[nodiscard]] std::vector<std::string>
    expand(std::string_view token, std::set<std::string, std::less<>>& traversed) const {
        const bool negate = token.starts_with('-');
        const auto name = token.substr(negate ? 1 : 0);
        if (!name.starts_with('@')) {
            return {std::string{token}};
        }
        const auto group = name.substr(1);
        std::vector<std::string> found;
        const auto members = groups_.find(group);
        if (traversed.contains(group) || members == groups_.end() || members->second.empty()) {
            found.emplace_back(name);
        } else {
            traversed.emplace(group);
            for (const auto& member : members->second) {
                if (!member.starts_with('-')) {
                    auto more = expand(member, traversed);
                    found.insert(found.end(), std::make_move_iterator(more.begin()),
                                 std::make_move_iterator(more.end()));
                }
            }
        }
        if (negate) {
            for (auto& each : found) {
                each.insert(0, 1, '-');
            }
        }
        return found;
    }

    // One source's package.* entries as grabdict_package reads them: a line for an atom
    // already read adds its tokens to the atom's.
    [[nodiscard]] KeyDict source(Range range) const {
        KeyDict found;
        for (const auto id : ids(range)) {
            StackedKey key{
                .atom = std::string{index_->string(entry(id).atom)}, .tokens = {}, .entries = {id}};
            for (const auto token : index_->ids_in(entry(id).tokens)) {
                key.tokens.push_back({.token = std::string{index_->string(token)}, .entry = id});
            }
            found.extend(std::move(key));
        }
        return found;
    }

    // KeywordsManager's pkeywordsdict: package.keywords and package.accept_keywords merged, an
    // empty key accepting the ~ form of each stable keyword of the profiles' make.defaults.
    [[nodiscard]] std::vector<StackedKey> user_keywords() const {
        const auto& ledger = index_->ledger;
        auto merged = source(ledger.package_keywords);
        auto accepted = source(ledger.package_accept_keywords);
        for (auto& key : accepted.keys()) {
            merged.extend(std::move(key));
        }
        std::vector<std::uint32_t> defaults;
        for (const auto& node : ledger.profiles) {
            const auto more = ids(node.defaults);
            defaults.insert(defaults.end(), more.begin(), more.end());
        }
        std::vector<std::string> stable;
        for (const auto& token : tokens(defaults, accept_keywords_var)) {
            if (!token.token.starts_with('~') && !token.token.starts_with('-')) {
                stable.push_back("~" + token.token);
            }
        }
        for (auto& key : merged.keys()) {
            if (key.tokens.empty()) {
                for (const auto& keyword : stable) {
                    key.tokens.push_back({.token = keyword, .entry = key.entries.front()});
                }
            }
        }
        return std::move(merged).ordered();
    }

    [[nodiscard]] std::vector<StackedMask> lines(Range range) const {
        std::vector<StackedMask> found;
        for (const auto id : ids(range)) {
            found.push_back({.atom = std::string{index_->string(entry(id).atom)}, .entry = id});
        }
        return found;
    }

    // MaskManager: each repository's own, stacked over each of its masters' for package.mask,
    // with ::repo; the profiles', stacked; the user's; all three stacked, a removal without a
    // repository removing the atom with one.
    [[nodiscard]] std::vector<StackedMask> masks(bool unmask) const {
        const auto& ledger = index_->ledger;
        const auto field = [&](const auto& source) {
            return unmask ? source.package_unmask : source.package_mask;
        };
        std::vector<StackedMask> repo_lines;
        for (const auto& repo : ledger.repositories) {
            const auto own = lines(field(repo));
            // stack_lists over (atom, source) pairs: the same atom from another line stays.
            std::vector<StackedMask> unioned;
            const auto add = [&](const std::vector<StackedMask>& stacked) {
                for (const auto& mask : stacked) {
                    if (!std::ranges::any_of(unioned, [&](const auto& kept) {
                            return kept.atom == mask.atom && kept.entry == mask.entry;
                        })) {
                        unioned.push_back(mask);
                    }
                }
            };
            const auto masters = index_->ids_in(repo.masters);
            if (unmask || masters.empty()) {
                AtomStack stacked;
                stacked.add(own, true, false);
                add(stacked.items());
            } else {
                for (const auto master : masters) {
                    const auto name = index_->string(master);
                    const auto found =
                        std::ranges::find_if(ledger.repositories, [&](const auto& r) {
                            return index_->string(r.name) == name;
                        });
                    AtomStack stacked;
                    if (found != ledger.repositories.end()) {
                        stacked.add(lines(field(*found)), true, false);
                    }
                    stacked.add(own, true, false);
                    add(stacked.items());
                }
            }
            const auto repo_name = index_->string(repo.name);
            for (auto& mask : unioned) {
                repo_lines.push_back(
                    {.atom = with_repo(mask.atom, repo_name), .entry = mask.entry});
            }
        }
        AtomStack profiles;
        for (const auto& node : ledger.profiles) {
            profiles.add(lines(field(node)), true, false);
        }
        AtomStack all;
        all.add(repo_lines, true, true);
        all.add(profiles.items(), true, true);
        all.add(lines(field(ledger)), true, true);
        return by_cp(all.items(),
                     [](const StackedMask& mask) -> std::string_view { return mask.atom; });
    }

    const RepositoryIndex* index_;
    std::map<std::string, std::set<std::string>, std::less<>> groups_;
};

} // namespace

StackedVisibility stack_visibility(const RepositoryIndex& index) {
    return Stacker{index}.stack();
}

} // namespace egraph
