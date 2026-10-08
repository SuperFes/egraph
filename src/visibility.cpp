#include "visibility.hpp"

#include "atom.hpp"
#include "version.hpp"
#include "visibility_stack.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace egraph {

namespace {

template <class T> const T& element(std::span<const T> items, std::size_t index) {
    return items.subspan(index, 1).front();
}

// What a configuration atom is matched against: one version as portage's _pkg_str has it.
struct Subject {
    std::string_view cp;
    std::string_view cpv;
    std::optional<Version> version;
    std::string_view slot;
    std::string_view sub_slot;
    std::string_view repo;
};

// portage's extended atom syntax, allowed in the user's package.* files: a cp in which * stands
// for anything but a /, with an optional slot and repository.
struct Wildcard {
    std::string cp;
    std::optional<std::string> slot;
    std::optional<std::string> sub_slot;
    std::optional<std::string> repo;
};

bool is_wildcard_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '+' || c == '.' || c == '-' || c == '*';
}

// The simple form only: =cat/pkg-*ver* (portage's "star" form) is read as matching nothing.
std::optional<Wildcard> parse_wildcard(std::string_view text) {
    Wildcard found;
    if (const auto at = text.find("::"); at != std::string_view::npos) {
        found.repo = std::string{text.substr(at + 2)};
        text = text.substr(0, at);
    }
    if (const auto at = text.find(':'); at != std::string_view::npos) {
        const auto slot = text.substr(at + 1);
        const auto slash = slot.find('/');
        found.slot = std::string{slot.substr(0, slash)};
        if (slash != std::string_view::npos) {
            found.sub_slot = std::string{slot.substr(slash + 1)};
        }
        text = text.substr(0, at);
    }
    if (std::ranges::count(text, '/') != 1 || text.contains("**") ||
        !std::ranges::all_of(text, [](char c) { return c == '/' || is_wildcard_char(c); })) {
        return std::nullopt;
    }
    found.cp = std::string{text};
    return found;
}

// extended_cp_match: * matches any run of characters but /.
bool glob_matches(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
        return text.empty();
    }
    if (pattern.front() == '*') {
        for (std::size_t skip = 0;; ++skip) {
            if (glob_matches(pattern.substr(1), text.substr(skip))) {
                return true;
            }
            if (skip == text.size() || text.at(skip) == '/') {
                return false;
            }
        }
    }
    return !text.empty() && text.front() == pattern.front() &&
           glob_matches(pattern.substr(1), text.substr(1));
}

// An atom of a package.* file or of package.mask.
struct ConfigAtom {
    std::optional<Atom> plain;
    std::optional<Wildcard> wildcard;

    explicit ConfigAtom(std::string_view text) {
        if (auto atom = parse_atom(text)) {
            plain = std::move(*atom);
        } else {
            wildcard = parse_wildcard(text);
        }
    }

    // match_from_list.
    [[nodiscard]] bool matches(const Subject& pkg) const {
        if (plain) {
            return pkg.version &&
                   egraph::matches(*plain, pkg.cp, *pkg.version, pkg.slot, pkg.sub_slot, pkg.repo);
        }
        return wildcard && glob_matches(wildcard->cp, pkg.cp) &&
               (!wildcard->slot ||
                (pkg.slot == *wildcard->slot &&
                 (!wildcard->sub_slot || pkg.sub_slot == *wildcard->sub_slot))) &&
               (!wildcard->repo || pkg.repo == *wildcard->repo);
    }

    // The cp a plain atom is filed under; empty for a wildcard.
    [[nodiscard]] std::string_view cp() const {
        return plain ? std::string_view{plain->cp} : std::string_view{};
    }
};

int operator_value(Operator op) {
    switch (op) {
    case Operator::equal:
        return 6;
    case Operator::approximately:
        return 5;
    case Operator::glob:
        return 4;
    case Operator::less:
    case Operator::less_equal:
    case Operator::greater:
    case Operator::greater_equal:
        return 2;
    case Operator::none:
        break;
    }
    return 1;
}

std::string atom_cpv(const Atom& atom) {
    return atom.version ? atom.cp + "-" + atom.version->text : atom.cp;
}

// best_match_to_list: the most specific of candidates that matches pkg, as an index into it.
std::optional<std::size_t> best_match(const Subject& pkg,
                                      std::span<const ConfigAtom* const> candidates) {
    int best_value = -99;
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = *element(candidates, i);
        if (!candidate.matches(pkg)) {
            continue;
        }
        if (!candidate.plain) {
            const int value = candidate.wildcard->slot ? -1 : -2;
            if (best_value < value) {
                best_value = value;
                best = i;
            }
            continue;
        }
        const auto& atom = *candidate.plain;
        if (atom.slot && best_value < 3) {
            best_value = 3;
            best = i;
        }
        const int value = operator_value(atom.op);
        if (value > best_value) {
            best_value = value;
            best = i;
        } else if (value == best_value && value == 2) {
            // For the range operators, the one whose version is closest to pkg's.
            const auto& best_atom = *element(candidates, *best)->plain;
            const auto best_cpv = atom_cpv(best_atom);
            const auto cpv = atom_cpv(atom);
            if (best_cpv == pkg.cpv || best_cpv == cpv) {
                continue;
            }
            if (cpv == pkg.cpv) {
                best = i;
                continue;
            }
            if (!pkg.version || !best_atom.version || !atom.version) {
                continue;
            }
            // Sorted stably by version: best, pkg, candidate.
            std::array<std::pair<const Version*, int>, 3> sorted{
                {{&*best_atom.version, 0}, {&*pkg.version, 1}, {&*atom.version, 2}}};
            std::ranges::stable_sort(sorted, [](const auto& a, const auto& b) {
                return vercmp(*a.first, *b.first) < 0;
            });
            if ((sorted.at(0).second == 1 || sorted.at(2).second == 1) &&
                sorted.at(1).second == 2) {
                best = i;
            }
        }
    }
    return best;
}

std::vector<std::string_view>
views_of(const std::vector<SourcedToken>& tokens EGRAPH_LIFETIMEBOUND) {
    std::vector<std::string_view> found;
    found.reserve(tokens.size());
    for (const auto& token : tokens) {
        found.emplace_back(token.token);
    }
    return found;
}

// A package.* file as portage's dicts hold it: entries filed by cp, then the wildcard ones,
// which an ExtendedAtomDict lookup appends. It views keys' tokens.
class EntryList {
  public:
    explicit EntryList(const std::vector<StackedKey>& keys EGRAPH_LIFETIMEBOUND) {
        for (const auto& key : keys) {
            atoms_.emplace_back(key.atom);
            tokens_.push_back(views_of(key.tokens));
        }
        for (std::size_t i = 0; i < atoms_.size(); ++i) {
            if (atoms_.at(i).plain) {
                by_cp_[atoms_.at(i).cp()].push_back(i);
            } else {
                wildcards_.push_back(i);
            }
        }
    }

    [[nodiscard]] bool empty() const { return atoms_.empty(); }

    // ordered_by_atom_specificity: the token lists of the entries matching pkg, the most
    // specific last.
    [[nodiscard]] std::vector<std::span<const std::string_view>>
    ordered(const Subject& pkg) const EGRAPH_LIFETIMEBOUND {
        std::vector<std::size_t> keys;
        if (const auto found = by_cp_.find(pkg.cp); found != by_cp_.end()) {
            keys = found->second;
        }
        for (const auto i : wildcards_) {
            if (glob_matches(atoms_.at(i).wildcard ? atoms_.at(i).wildcard->cp : "", pkg.cp)) {
                keys.push_back(i);
            }
        }
        std::vector<std::span<const std::string_view>> found;
        while (!keys.empty()) {
            std::vector<const ConfigAtom*> candidates;
            candidates.reserve(keys.size());
            for (const auto i : keys) {
                candidates.push_back(&atoms_.at(i));
            }
            const auto best = best_match(pkg, candidates);
            if (!best) {
                break;
            }
            found.emplace_back(tokens_.at(keys.at(*best)));
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(*best));
        }
        std::ranges::reverse(found);
        return found;
    }

  private:
    std::vector<ConfigAtom> atoms_;
    std::vector<std::vector<std::string_view>> tokens_;
    std::map<std::string_view, std::vector<std::size_t>, std::less<>> by_cp_;
    std::vector<std::size_t> wildcards_;
};

// package.mask or package.unmask, as an ExtendedAtomDict of lists.
class AtomList {
  public:
    explicit AtomList(const std::vector<StackedMask>& masks) {
        for (const auto& mask : masks) {
            ConfigAtom atom{mask.atom};
            if (atom.plain) {
                const auto cp = atom.cp();
                by_cp_[std::string{cp}].push_back(std::move(atom));
            } else {
                wildcards_.push_back(std::move(atom));
            }
        }
    }

    // Whether any atom filed under pkg's cp, or any wildcard one, matches it.
    [[nodiscard]] bool any_matches(const Subject& pkg) const {
        const auto matches = [&](const ConfigAtom& atom) { return atom.matches(pkg); };
        const auto found = by_cp_.find(pkg.cp);
        return (found != by_cp_.end() && std::ranges::any_of(found->second, matches)) ||
               std::ranges::any_of(wildcards_, matches);
    }

    // Whether no atom could match a version of cp.
    [[nodiscard]] bool none_for(std::string_view cp) const {
        return wildcards_.empty() && !by_cp_.contains(cp);
    }

  private:
    std::map<std::string, std::vector<ConfigAtom>, std::less<>> by_cp_;
    std::vector<ConfigAtom> wildcards_;
};

// stack_lists(incremental=True) over token lists: -* clears, -x removes x, x adds it once, in
// the order first added.
void stack(std::vector<std::string_view>& stacked, std::span<const std::string_view> tokens) {
    for (const auto token : tokens) {
        if (token == "-*") {
            stacked.clear();
        } else if (token.starts_with('-')) {
            std::erase(stacked, token.substr(1));
        } else if (!std::ranges::contains(stacked, token)) {
            stacked.push_back(token);
        }
    }
}

// KeywordsManager._getEgroups: the accepted keywords, incrementally.
std::set<std::string_view> incremental_set(std::span<const std::string_view> tokens) {
    std::set<std::string_view> found;
    for (const auto token : tokens) {
        if (token == "-*") {
            found.clear();
        } else if (token.starts_with('-')) {
            found.erase(token.substr(1));
        } else {
            found.insert(token);
        }
    }
    return found;
}

// What an ACCEPT_LICENSE-like list accepts of a package's names, where * stands for every one of
// them: the names it adds last, and with all, every other one it does not remove last.
class Acceptance {
  public:
    explicit Acceptance(std::span<const std::string_view> tokens) { add(tokens); }

    void add(std::span<const std::string_view> tokens) {
        for (const auto token : tokens) {
            if (token == "*" || token == "-*") {
                all_ = token == "*";
                added_.clear();
                removed_.clear();
            } else if (token.starts_with('-')) {
                added_.erase(token.substr(1));
                removed_.insert(token.substr(1));
            } else {
                removed_.erase(token);
                added_.insert(token);
            }
        }
    }

    [[nodiscard]] bool accepts(std::string_view name) const {
        return added_.contains(name) || (all_ && !removed_.contains(name));
    }

  private:
    bool all_ = false;
    std::set<std::string_view, std::less<>> added_;
    std::set<std::string_view, std::less<>> removed_;
};

// A LICENSE-like string reduced under USE, groups kept as use_reduce's opconvert keeps them: a
// conditional group's contents join the group around it.
struct Group {
    bool any_of = false;
    std::vector<std::string_view> names;
    std::vector<Group> groups;
};

bool is_conditional(std::string_view token) {
    return token.size() > 1 && token.ends_with('?');
}

// Reads tokens from pos into group up to its ")" (consumed) or, at the top, the end. With
// matchall every conditional holds; otherwise flag? holds when use has flag, !flag? when not.
std::optional<std::string> parse_group(std::span<const std::string_view> tokens, std::size_t& pos,
                                       Group& group, const std::set<std::string_view>& use,
                                       bool matchall, bool top) {
    while (pos < tokens.size()) {
        const auto token = element(tokens, pos++);
        if (token == ")") {
            return top ? std::optional<std::string>{"unmatched ')'"} : std::nullopt;
        }
        if (token == "(" || token == "||" || is_conditional(token)) {
            if (token != "(" && (pos >= tokens.size() || element(tokens, pos++) != "(")) {
                return std::format("'{}' without a '(' after it", token);
            }
            if (is_conditional(token)) {
                const auto flag = token.substr(token.starts_with('!') ? 1 : 0,
                                               token.size() - (token.starts_with('!') ? 2 : 1));
                const bool holds = matchall || (use.contains(flag) != token.starts_with('!'));
                Group inner;
                if (auto error = parse_group(tokens, pos, inner, use, matchall, false)) {
                    return error;
                }
                if (holds) {
                    group.names.insert(group.names.end(), inner.names.begin(), inner.names.end());
                    for (auto& nested : inner.groups) {
                        group.groups.push_back(std::move(nested));
                    }
                }
                continue;
            }
            Group inner{.any_of = token == "||", .names = {}, .groups = {}};
            if (auto error = parse_group(tokens, pos, inner, use, matchall, false)) {
                return error;
            }
            group.groups.push_back(std::move(inner));
            continue;
        }
        group.names.push_back(token);
    }
    return top ? std::nullopt : std::optional<std::string>{"missing ')'"};
}

std::expected<Group, std::string> reduce(std::span<const std::string_view> tokens,
                                         const std::set<std::string_view>& use, bool matchall) {
    Group top;
    std::size_t pos = 0;
    if (auto error = parse_group(tokens, pos, top, use, matchall, true)) {
        return std::unexpected(std::move(*error));
    }
    return top;
}

// Every name of a reduced string, in order; with operators, each group's || as use_reduce's
// flat output keeps it.
void flatten(const Group& group, std::vector<std::string_view>& out, bool operators) {
    if (operators && group.any_of) {
        out.emplace_back("||");
    }
    out.insert(out.end(), group.names.begin(), group.names.end());
    for (const auto& nested : group.groups) {
        flatten(nested, out, operators);
    }
}

// LicenseManager._getMaskedLicenses.
void masked_licenses(const Group& group, const Acceptance& accepted,
                     std::set<std::string_view>& out) {
    if (group.any_of) {
        std::set<std::string_view> found;
        for (const auto name : group.names) {
            if (accepted.accepts(name)) {
                return;
            }
            found.insert(name);
        }
        for (const auto& nested : group.groups) {
            std::set<std::string_view> inner;
            masked_licenses(nested, accepted, inner);
            if (inner.empty()) {
                return;
            }
            found.insert(inner.begin(), inner.end());
        }
        out.insert(found.begin(), found.end());
        return;
    }
    for (const auto name : group.names) {
        if (!accepted.accepts(name)) {
            out.insert(name);
        }
    }
    for (const auto& nested : group.groups) {
        masked_licenses(nested, accepted, out);
    }
}

std::vector<std::string_view> strings_of(const RepositoryIndex& index, Range range) {
    std::vector<std::string_view> found;
    for (const auto id : index.ids_in(range)) {
        found.push_back(index.string(id));
    }
    return found;
}

bool has_conditional(std::span<const std::string_view> tokens) {
    return std::ranges::any_of(tokens, is_conditional);
}

} // namespace

struct VersionMasks::Rules {
    std::reference_wrapper<const RepositoryIndex> index;
    // What the lists below view; Rules is never moved.
    StackedVisibility visibility;
    std::map<std::string_view, Eapi, std::less<>> eapis;
    // Sorted, as settings["ACCEPT_KEYWORDS"] is.
    std::vector<std::string_view> accept_keywords;
    // What an empty package.accept_keywords line in a profile accepts: the ~ form of each
    // stable keyword ACCEPT_KEYWORDS accepts.
    std::vector<std::string> tilde_defaults;
    std::vector<std::string_view> environment_keywords;
    std::string_view arch;
    std::vector<EntryList> profile_keywords;
    std::vector<EntryList> profile_accept_keywords;
    EntryList accept_keywords_entries;
    AtomList masks;
    AtomList unmasks;
    Acceptance accept_license;
    EntryList licenses;
    Acceptance accept_properties;
    EntryList properties;
    Acceptance accept_restrict;
    EntryList restrict;
    // ACCEPT_KEYWORDS with the environment's, for a package no package.accept_keywords line
    // names.
    std::set<std::string_view> global_keywords;

    explicit Rules(const RepositoryIndex& from)
        : index{from}, visibility{stack_visibility(from)},
          accept_keywords{views_of(visibility.accept_keywords)},
          environment_keywords{views_of(visibility.environment_keywords)},
          arch{from.string(from.visibility.arch)},
          accept_keywords_entries{visibility.accept_keywords_entries}, masks{visibility.masks},
          unmasks{visibility.unmasks}, accept_license{views_of(visibility.accept_license)},
          licenses{visibility.licenses}, accept_properties{views_of(visibility.accept_properties)},
          properties{visibility.properties}, accept_restrict{views_of(visibility.accept_restrict)},
          restrict {
        visibility.restrict
    }
    {
        std::ranges::sort(accept_keywords);
        for (const auto& eapi : from.visibility.eapis) {
            eapis.emplace(from.string(eapi.eapi), eapi);
        }
        for (const auto keyword : accept_keywords) {
            if (!keyword.starts_with('~') && !keyword.starts_with('-')) {
                tilde_defaults.push_back("~" + std::string{keyword});
            }
        }
        auto groups = accept_keywords;
        if (environment_keywords.empty()) {
            global_keywords = {groups.begin(), groups.end()};
        } else {
            groups.insert(groups.end(), environment_keywords.begin(), environment_keywords.end());
            global_keywords = incremental_set(groups);
        }
        // A layer per profile node, most of them empty.
        for (const auto& layer : visibility.profile_keywords) {
            if (!layer.empty()) {
                profile_keywords.emplace_back(layer);
            }
        }
        for (const auto& layer : visibility.profile_accept_keywords) {
            if (!layer.empty()) {
                profile_accept_keywords.emplace_back(layer);
            }
        }
    }

    [[nodiscard]] Subject subject(const IndexVersion& version) const {
        const auto& from = index.get();
        const auto cp = from.string(version.cp);
        const auto cpv = from.string(version.cpv);
        return {.cp = cp,
                .cpv = cpv,
                .version = parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1))),
                .slot = from.string(version.slot),
                .sub_slot = from.string(version.sub_slot),
                .repo = from.string(from.repositories.at(version.repository).name)};
    }

    // Whether the EAPI keeps the version out: unsupported, or deprecated.
    [[nodiscard]] bool eapi_masks(const IndexVersion& version) const {
        const auto found = eapis.find(index.get().string(version.eapi));
        return found == eapis.end() || !found->second.supported || found->second.deprecated;
    }

    // MaskManager.getMaskAtom: a package.mask atom matches, and no package.unmask one does.
    [[nodiscard]] bool mask_matches(const Subject& pkg) const {
        return masks.any_matches(pkg) && !unmasks.any_matches(pkg);
    }

    // KeywordsManager.getKeywords: KEYWORDS without -*, stacked with the profiles'
    // package.keywords.
    [[nodiscard]] std::vector<std::string_view> keywords(const IndexVersion& version,
                                                         const Subject& pkg) const {
        std::vector<std::string_view> own = strings_of(index.get(), version.keywords);
        std::erase(own, "-*");
        std::vector<std::string_view> stacked;
        stack(stacked, own);
        for (const auto& layer : profile_keywords) {
            for (const auto tokens : layer.ordered(pkg)) {
                stack(stacked, tokens);
            }
        }
        return stacked;
    }

    // The keywords accepted for pkg: ACCEPT_KEYWORDS with what package.accept_keywords adds,
    // the environment's stacked last. global_keywords, or scratch filled for pkg.
    [[nodiscard]] const std::set<std::string_view>& accepted_keywords(
        const Subject& pkg,
        std::set<std::string_view>& scratch EGRAPH_LIFETIMEBOUND) const EGRAPH_LIFETIMEBOUND {
        std::vector<std::string_view> unmask;
        for (const auto& layer : profile_accept_keywords) {
            for (const auto tokens : layer.ordered(pkg)) {
                if (tokens.empty()) {
                    unmask.insert(unmask.end(), tilde_defaults.begin(), tilde_defaults.end());
                } else {
                    unmask.insert(unmask.end(), tokens.begin(), tokens.end());
                }
            }
        }
        for (const auto tokens : accept_keywords_entries.ordered(pkg)) {
            unmask.insert(unmask.end(), tokens.begin(), tokens.end());
        }
        if (unmask.empty()) {
            return global_keywords;
        }
        std::vector<std::string_view> groups = accept_keywords;
        groups.insert(groups.end(), unmask.begin(), unmask.end());
        groups.insert(groups.end(), environment_keywords.begin(), environment_keywords.end());
        scratch = incremental_set(groups);
        return scratch;
    }

    // KeywordsManager._getMissingKeywords: whether nothing accepted covers the keywords.
    [[nodiscard]] static bool keywords_missing(const std::set<std::string_view>& accepted,
                                               std::span<const std::string_view> keywords) {
        bool stable = false;
        bool testing = false;
        for (const auto keyword : keywords) {
            if (keyword == "*") {
                return false;
            }
            if (keyword == "~*") {
                testing = true;
                if (std::ranges::any_of(accepted, [](auto x) { return x.starts_with('~'); })) {
                    return false;
                }
            } else if (accepted.contains(keyword)) {
                return false;
            } else if (keyword.starts_with('~')) {
                testing = true;
            } else if (!keyword.starts_with('-')) {
                stable = true;
            }
        }
        return !((testing && accepted.contains("~*")) || (stable && accepted.contains("*")) ||
                 accepted.contains("**"));
    }

    // What a package.* kind accepts of pkg: the global list, then each matching entry's;
    // global itself, or scratch filled for pkg.
    [[nodiscard]] static const Acceptance&
    accepted_for(const Acceptance& global EGRAPH_LIFETIMEBOUND, const EntryList& entries,
                 const Subject& pkg, std::optional<Acceptance>& scratch EGRAPH_LIFETIMEBOUND) {
        const auto lists = entries.ordered(pkg);
        if (lists.empty()) {
            return global;
        }
        scratch = global;
        for (const auto tokens : lists) {
            scratch->add(tokens);
        }
        return *scratch;
    }

    // LicenseManager.getMissingLicenses, under use.
    [[nodiscard]] std::expected<std::set<std::string_view>, std::string>
    missing_licenses(const IndexVersion& version, const Subject& pkg,
                     const std::set<std::string_view>& use) const {
        const auto tokens = strings_of(index.get(), version.license);
        std::optional<Acceptance> scratch;
        const auto& accepted = accepted_for(accept_license, licenses, pkg, scratch);
        const auto reduced = reduce(tokens, use, false);
        if (!reduced) {
            return std::unexpected(reduced.error());
        }
        std::set<std::string_view> missing;
        masked_licenses(*reduced, accepted, missing);
        return missing;
    }

    // config._getMissingProperties and _getMissingRestrict, under use.
    [[nodiscard]] static std::expected<std::vector<std::string_view>, std::string>
    missing_tokens(std::span<const std::string_view> tokens, const Acceptance& global,
                   const EntryList& entries, const Subject& pkg,
                   const std::set<std::string_view>& use) {
        if (tokens.empty()) {
            return {};
        }
        std::optional<Acceptance> scratch;
        const auto& accepted = accepted_for(global, entries, pkg, scratch);
        const auto reduced = reduce(tokens, use, false);
        if (!reduced) {
            return std::unexpected(reduced.error());
        }
        std::vector<std::string_view> found;
        flatten(*reduced, found, true);
        std::erase_if(found, [&](auto token) { return accepted.accepts(token); });
        return found;
    }

    [[nodiscard]] std::set<std::string_view> use_of(const IndexVersion& version) const {
        const auto flags = strings_of(index.get(), version.use);
        return {flags.begin(), flags.end()};
    }
};

VersionMasks::VersionMasks(const RepositoryIndex& index)
    : rules_{std::make_unique<const Rules>(index)} {}
VersionMasks::VersionMasks(VersionMasks&&) noexcept = default;
VersionMasks& VersionMasks::operator=(VersionMasks&&) noexcept = default;
VersionMasks::~VersionMasks() = default;

bool VersionMasks::visible(std::uint32_t id) const {
    return rules_->index.get().versions.at(id).invalid.count == 0 && portdb_visible(id);
}

bool VersionMasks::portdb_visible(std::uint32_t id) const {
    const auto& rules = *rules_;
    const auto& index = rules.index.get();
    const auto& version = index.versions.at(id);
    if (rules.eapi_masks(version) || index.string(version.slot).empty()) {
        return false;
    }
    const auto pkg = rules.subject(version);
    std::set<std::string_view> scratch;
    if (rules.mask_matches(pkg) || Rules::keywords_missing(rules.accepted_keywords(pkg, scratch),
                                                           rules.keywords(version, pkg))) {
        return false;
    }
    // portdbapi reads the ebuild's USE only for a LICENSE or PROPERTIES conditional, and then
    // reduces RESTRICT's under it too; the index holds it only then.
    const auto use = rules.use_of(version);
    const auto licenses = rules.missing_licenses(version, pkg, use);
    if (!licenses || !licenses->empty()) {
        return false;
    }
    for (const auto& [tokens, global, entries] :
         {std::tuple{version.properties, std::cref(rules.accept_properties),
                     std::cref(rules.properties)},
          std::tuple{version.restrict, std::cref(rules.accept_restrict),
                     std::cref(rules.restrict)}}) {
        const auto missing =
            Rules::missing_tokens(strings_of(index, tokens), global.get(), entries.get(), pkg, use);
        if (!missing || !missing->empty()) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> VersionMasks::reasons(std::uint32_t id) const {
    const auto& index = rules_->index.get();
    auto found = portdb_visible(id) ? std::vector<std::string>{} : masking_status(id);
    for (const auto message : index.ids_in(index.versions.at(id).invalid)) {
        found.push_back(std::format("invalid: {}", index.string(message)));
    }
    if (index.string(index.versions.at(id).slot).empty()) {
        found.emplace_back("SLOT: undefined");
    }
    return found;
}

std::vector<std::string> VersionMasks::masking_status(std::uint32_t id) const {
    const auto& rules = *rules_;
    const auto& index = rules.index.get();
    const auto& version = index.versions.at(id);
    const auto pkg = rules.subject(version);
    std::vector<std::string> found;
    if (rules.mask_matches(pkg)) {
        found.emplace_back("package.mask");
    }
    if (rules.eapi_masks(version)) {
        return {std::format("EAPI {}", index.string(version.eapi))};
    }

    const auto keywords = rules.keywords(version, pkg);
    auto arch = rules.arch;
    if (!rules.accept_keywords.empty() && !std::ranges::contains(rules.accept_keywords, arch)) {
        arch = rules.accept_keywords.front();
        arch.remove_prefix(arch.starts_with('~') ? 1 : 0);
    }
    std::set<std::string_view> scratch;
    const auto& accepted = rules.accepted_keywords(pkg, scratch);
    std::optional<std::string> kmask = "missing";
    if (accepted.contains("**") ||
        std::ranges::any_of(accepted, [&](auto k) { return std::ranges::contains(keywords, k); })) {
        kmask.reset();
    }
    if (kmask) {
        for (const auto keyword : keywords) {
            if (keyword == "*") {
                kmask.reset();
                break;
            }
            if (keyword == "~*") {
                if (std::ranges::any_of(accepted, [](auto x) { return x.starts_with('~'); })) {
                    kmask.reset();
                    break;
                }
            } else if (accepted.contains(arch) && (keyword == std::format("-{}", arch) ||
                                                   keyword == std::format("~{}", arch))) {
                kmask = std::string{keyword};
                break;
            }
        }
    }

    // getmaskingstatus reads the ebuild's USE only for a LICENSE conditional.
    const auto license_tokens = strings_of(index, version.license);
    const auto use =
        has_conditional(license_tokens) ? rules.use_of(version) : std::set<std::string_view>{};
    const auto words = [](std::span<const std::string_view> tokens, std::string_view last) {
        std::string text;
        for (const auto token : tokens) {
            text += std::string{token} + " ";
        }
        return text + std::string{last};
    };
    if (const auto missing = rules.missing_licenses(version, pkg, use); !missing) {
        found.push_back(std::format("LICENSE: {}", missing.error()));
    } else if (!missing->empty()) {
        std::vector<std::string_view> shown;
        for (const auto token : license_tokens) {
            if (token == "||" || token == "(" || token == ")" || missing->contains(token)) {
                shown.push_back(token);
            }
        }
        found.push_back(words(shown, "license(s)"));
    }
    const auto property_tokens = strings_of(index, version.properties);
    if (const auto missing = Rules::missing_tokens(property_tokens, rules.accept_properties,
                                                   rules.properties, pkg, use);
        !missing) {
        found.push_back(std::format("PROPERTIES: {}", missing.error()));
    } else if (!missing->empty()) {
        const std::set<std::string_view> missed{missing->begin(), missing->end()};
        std::vector<std::string_view> shown;
        for (const auto token : property_tokens) {
            if (token == "||" || token == "(" || token == ")" || missed.contains(token)) {
                shown.push_back(token);
            }
        }
        found.push_back(words(shown, "properties"));
    }
    if (const auto missing = Rules::missing_tokens(strings_of(index, version.restrict),
                                                   rules.accept_restrict, rules.restrict, pkg, use);
        !missing) {
        found.push_back(std::format("RESTRICT: {}", missing.error()));
    } else if (!missing->empty()) {
        found.push_back(words(*missing, "in RESTRICT"));
    }
    if (kmask) {
        found.push_back(*kmask + " keyword");
    }
    return found;
}

} // namespace egraph

namespace egraph {

std::expected<std::vector<std::string>, std::string>
version_lines(const RepositoryIndex& index, const VersionMasks& masking,
              std::span<const std::string> arguments) {
    // An atom, or a name without a category.
    std::vector<std::variant<Atom, std::string_view>> wanted;
    for (const auto& argument : arguments) {
        if (!argument.contains('/')) {
            wanted.emplace_back(std::string_view{argument});
            continue;
        }
        auto atom = parse_atom(argument);
        if (!atom) {
            return std::unexpected(atom.error());
        }
        if (!atom->use.empty()) {
            return std::unexpected(std::format("{}: versions take no USE dependencies", argument));
        }
        wanted.emplace_back(std::move(*atom));
    }
    std::vector<bool> named(wanted.size(), false);
    std::vector<std::uint32_t> chosen;
    for (std::uint32_t id = 0; id < index.versions.size(); ++id) {
        const auto& version = index.versions.at(id);
        const auto cp = index.string(version.cp);
        const auto cpv = index.string(version.cpv);
        const auto parsed = parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
        const auto repo = index.string(index.repositories.at(version.repository).name);
        const auto hits = [&](const auto& each) {
            using T = std::decay_t<decltype(each)>;
            if constexpr (std::is_same_v<T, Atom>) {
                return parsed && matches(each, cp, *parsed, index.string(version.slot),
                                         index.string(version.sub_slot), repo);
            } else {
                return cp.substr(cp.find('/') + 1) == each;
            }
        };
        bool kept = wanted.empty();
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (std::visit(hits, wanted.at(i))) {
                named.at(i) = true;
                kept = true;
            }
        }
        if (kept) {
            chosen.push_back(id);
        }
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (!named.at(i)) {
            return std::unexpected(
                std::format("{}: no version in the repositories", arguments.subspan(i, 1).front()));
        }
    }
    const auto version_of = [&index](std::uint32_t id) {
        const auto& version = index.versions.at(id);
        const auto cp = index.string(version.cp);
        const auto cpv = index.string(version.cpv);
        return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
    };
    std::vector<std::optional<Version>> parsed(index.versions.size());
    for (const auto id : chosen) {
        parsed.at(id) = version_of(id);
    }
    // The index holds a cp's versions by repository; these go by version.
    std::ranges::stable_sort(chosen, [&](std::uint32_t a, std::uint32_t b) {
        const auto& left = index.versions.at(a);
        const auto& right = index.versions.at(b);
        if (left.cp != right.cp) {
            return index.string(left.cp) < index.string(right.cp);
        }
        const auto& va = parsed.at(a);
        const auto& vb = parsed.at(b);
        const int order = va && vb ? vercmp(*va, *vb) : 0;
        return order != 0 ? order < 0 : left.repository < right.repository;
    });
    std::vector<std::string> lines;
    for (const auto id : chosen) {
        const auto& version = index.versions.at(id);
        const auto slot = index.string(version.slot);
        const auto sub_slot = index.string(version.sub_slot);
        auto line = std::format("{}::{}\t{}{}", index.string(version.cpv),
                                index.string(index.repositories.at(version.repository).name), slot,
                                sub_slot == slot ? "" : std::format("/{}", sub_slot));
        if (masking.visible(id)) {
            line += "\tvisible";
        } else {
            line += "\tmasked";
            for (const auto& reason : masking.reasons(id)) {
                line += "\t" + reason;
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace egraph
