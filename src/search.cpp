#include "search.hpp"

#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <regex>
#include <utility>

namespace egraph {

namespace {

std::string lowered(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

struct Block {
    std::size_t a = 0;
    std::size_t b = 0;
    std::size_t size = 0;
};

// SequenceMatcher.find_longest_match without junk: the longest common run of a[alo:ahi] and
// b[blo:bhi], b given by where each byte is in it, the earliest in a, then in b.
Block longest_match(std::string_view a, const std::array<std::vector<std::size_t>, 256>& b2j,
                    std::size_t alo, std::size_t ahi, std::size_t blo, std::size_t bhi) {
    Block best{.a = alo, .b = blo, .size = 0};
    // Run lengths ending at each j of b for the previous i, and for this one.
    std::map<std::size_t, std::size_t> previous;
    for (auto i = alo; i < ahi; ++i) {
        std::map<std::size_t, std::size_t> current;
        for (const auto j : b2j.at(static_cast<unsigned char>(a.at(i)))) {
            if (j < blo) {
                continue;
            }
            if (j >= bhi) {
                break;
            }
            const auto before = j > 0 ? previous.find(j - 1) : previous.end();
            const auto k = (before == previous.end() ? 0 : before->second) + 1;
            current[j] = k;
            if (k > best.size) {
                best = {.a = i + 1 - k, .b = j + 1 - k, .size = k};
            }
        }
        previous = std::move(current);
    }
    return best;
}

std::optional<std::regex> compiled(std::string_view pattern) {
    // std::regex reports a pattern it cannot read only by throwing.
    try {
        return std::regex{std::string{pattern}, std::regex::ECMAScript | std::regex::icase};
    } catch (const std::regex_error&) {
        return std::nullopt;
    }
}

// What emerge's regex_auto takes for a regular expression: [\^\$\*\[\]\{\}\|\?]|\.\+
bool looks_like_regex(std::string_view key) {
    return key.find_first_of("^$*[]{}|?") != std::string_view::npos || key.contains(".+");
}

// portage.catsplit: the category and the name, or the whole when there is no "/".
std::vector<std::string_view> catsplit(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        return {text};
    }
    return {text.substr(0, slash), text.substr(slash + 1)};
}

std::string_view name_of(std::string_view cp) {
    return cp.substr(cp.find('/') + 1);
}

std::optional<Version> version_of(std::string_view cp, std::string_view cpv) {
    return parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
}

// portage's getVersion: the version with its revision, but for -r0.
std::vector<std::string> shown_reasons(const RepositoryIndex& index, const VersionMasks& masks,
                                       std::uint32_t id) {
    std::vector<std::string> found;
    for (const auto& reason : masks.sourced_reasons(id)) {
        found.push_back(shown_reason(index, reason));
    }
    return found;
}

std::string shown(const Version& version) {
    if (version.revision == "0") {
        return version.base;
    }
    return version.text;
}

} // namespace

double sequence_ratio(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) {
        return 1.0;
    }
    std::array<std::vector<std::size_t>, 256> b2j;
    for (std::size_t j = 0; j < b.size(); ++j) {
        b2j.at(static_cast<unsigned char>(b.at(j))).push_back(j);
    }
    std::size_t matches = 0;
    std::vector<std::array<std::size_t, 4>> queue{{0, a.size(), 0, b.size()}};
    while (!queue.empty()) {
        const auto [alo, ahi, blo, bhi] = queue.back();
        queue.pop_back();
        const auto found = longest_match(a, b2j, alo, ahi, blo, bhi);
        if (found.size == 0) {
            continue;
        }
        matches += found.size;
        if (alo < found.a && blo < found.b) {
            queue.push_back({alo, found.a, blo, found.b});
        }
        if (found.a + found.size < ahi && found.b + found.size < bhi) {
            queue.push_back({found.a + found.size, ahi, found.b + found.size, bhi});
        }
    }
    return 2.0 * static_cast<double>(matches) / static_cast<double>(a.size() + b.size());
}

std::expected<std::vector<std::string_view>, std::string>
search_cps(std::span<const std::string_view> cps,
           const std::function<std::string_view(std::string_view)>& description,
           std::string_view key, const SearchOptions& options) {
    bool regex = false;
    bool category = false;
    if (key.starts_with('%')) {
        regex = true;
        key.remove_prefix(1);
    }
    if (key.starts_with('@')) {
        category = true;
        key.remove_prefix(1);
    }
    category = category || key.contains('/');
    std::optional<std::regex> pattern;
    if (regex) {
        pattern = compiled(key);
        if (!pattern) {
            return std::unexpected(std::format("{}: not a regular expression", key));
        }
    } else if (options.regex_auto && looks_like_regex(key)) {
        pattern = compiled(key);
    }
    const auto lowered_key = lowered(key);
    const auto matches = [&](std::string_view text) {
        if (pattern) {
            return std::regex_search(text.begin(), text.end(), *pattern);
        }
        return lowered(text).contains(lowered_key);
    };
    const bool fuzzy = !pattern && options.fuzzy;
    const double cutoff = static_cast<double>(options.similarity) / 100;
    const auto key_parts =
        category ? catsplit(lowered_key) : std::vector<std::string_view>{lowered_key};
    const auto fuzzy_matches = [&](std::string_view text) {
        const auto lower = lowered(text);
        const auto parts =
            category ? catsplit(lower) : std::vector<std::string_view>{std::string_view{lower}};
        for (std::size_t i = 0; i < std::min(parts.size(), key_parts.size()); ++i) {
            if (sequence_ratio(parts.at(i), key_parts.at(i)) < cutoff) {
                return false;
            }
        }
        return true;
    };
    std::vector<std::string_view> found;
    for (const auto cp : cps) {
        const auto text = category ? cp : name_of(cp);
        if (matches(text) || (fuzzy && fuzzy_matches(text)) ||
            (options.description && matches(description(cp)))) {
            found.push_back(cp);
        }
    }
    return found;
}

Catalogue::Catalogue(const Store& installed, const Evaluated& evaluated,
                     const RepositoryIndex& index, const VersionMasks& masks)
    : installed_{installed}, evaluated_{evaluated}, index_{index}, masks_{masks} {
    for (std::uint32_t id = 0; id < index.versions.size(); ++id) {
        by_cp_[index.string(index.versions.at(id).cp)].ebuilds.push_back(id);
    }
    for (std::uint32_t id = 0; id < installed.packages.size(); ++id) {
        by_cp_[installed.string(installed.packages.at(id).cp)].installed.push_back(id);
    }
    for (const auto& [cp, _] : by_cp_) {
        cps_.push_back(cp);
    }
}

bool Catalogue::installed_visible(std::uint32_t package) const {
    const auto& evaluated = evaluated_.get();
    if (evaluated.packages.size() != installed_.get().packages.size()) {
        return true;
    }
    return evaluated.packages.at(package).vdb_hidden == Hidden::visible;
}

// _first_cp's through its IndexedPortdb: of the repositories with a pkg_desc_index, read lowest
// priority first, the first holding cp gives its lowest version, whose description is that of the
// last of them listing it, egencache's description of its highest version. Without one, the
// lowest version's own, from the repository of highest priority holding it.
std::string_view Catalogue::description(std::string_view cp) const {
    const auto& index = index_.get();
    const auto& ebuilds = by_cp_.at(cp).ebuilds;
    const auto version = [&](std::uint32_t id) {
        return version_of(cp, index.string(index.versions.at(id).cpv));
    };
    const auto repository = [&](std::uint32_t id) { return index.versions.at(id).repository; };
    const auto indexed = [&](std::uint32_t id) {
        return index.repositories.at(repository(id)).description_index;
    };
    // Versions as (version, id), and the extreme by vercmp, then by repository.
    const auto extreme = [&](const auto& keep, bool highest) {
        std::optional<std::pair<Version, std::uint32_t>> found;
        for (const auto id : ebuilds) {
            const auto parsed = version(id);
            if (!parsed || !keep(id)) {
                continue;
            }
            const int order = found ? vercmp(*parsed, found->first) : 0;
            if (!found || (highest ? order > 0 : order < 0) ||
                (order == 0 && repository(id) < repository(found->second))) {
                found = {*parsed, id};
            }
        }
        return found;
    };
    std::optional<std::uint32_t> first_indexed;
    for (const auto id : ebuilds) {
        if (indexed(id) && (!first_indexed || repository(id) > *first_indexed)) {
            first_indexed = repository(id);
        }
    }
    if (!first_indexed) {
        const auto lowest = extreme([](std::uint32_t) { return true; }, false);
        return lowest ? index.string(index.versions.at(lowest->second).description)
                      : std::string_view{};
    }
    const auto lowest =
        extreme([&](std::uint32_t id) { return repository(id) == *first_indexed; }, false);
    if (!lowest) {
        return {};
    }
    // The indexed repository of highest priority listing that version wrote it last.
    std::optional<std::uint32_t> last;
    for (const auto id : ebuilds) {
        const auto parsed = version(id);
        if (indexed(id) && parsed && vercmp(*parsed, lowest->first) == 0 &&
            (!last || repository(id) < *last)) {
            last = repository(id);
        }
    }
    const auto highest = extreme([&](std::uint32_t id) { return repository(id) == last; }, true);
    return highest ? index.string(index.versions.at(highest->second).description)
                   : std::string_view{};
}

std::expected<std::vector<std::string_view>, std::string>
Catalogue::search(std::string_view key, const SearchOptions& options) const {
    return search_cps(cps_, [this](std::string_view cp) { return description(cp); }, key, options);
}

Found Catalogue::found(std::string_view cp) const {
    const auto& found = by_cp_.at(cp);
    const auto& index = index_.get();
    const auto& store = installed_.get();
    // Best visible, else best of all: (version, ebuild id if one holds it).
    std::optional<std::pair<Version, std::optional<std::uint32_t>>> visible;
    std::optional<std::pair<Version, std::optional<std::uint32_t>>> any;
    // Of two ebuilds of one version, the one from the repository of higher priority.
    const auto earlier = [&index](std::uint32_t a, std::uint32_t b) {
        return index.versions.at(a).repository < index.versions.at(b).repository;
    };
    const auto consider = [&earlier](auto& best, const Version& version,
                                     std::optional<std::uint32_t> ebuild) {
        if (!best || vercmp(version, best->first) > 0) {
            best = {version, ebuild};
        } else if (vercmp(version, best->first) == 0 && ebuild &&
                   (!best->second || earlier(*ebuild, *best->second))) {
            best->second = ebuild;
        }
    };
    for (const auto id : found.ebuilds) {
        const auto version = version_of(cp, index.string(index.versions.at(id).cpv));
        if (!version) {
            continue;
        }
        consider(any, *version, id);
        if (masks_.get().visible(id)) {
            consider(visible, *version, id);
        }
    }
    std::optional<Version> installed_best;
    for (const auto id : found.installed) {
        const auto version = version_of(cp, store.string(store.packages.at(id).cpv));
        if (!version) {
            continue;
        }
        consider(any, *version, std::nullopt);
        if (installed_visible(id)) {
            consider(visible, *version, std::nullopt);
        }
        if (!installed_best || vercmp(*version, *installed_best) > 0) {
            installed_best = *version;
        }
    }
    const auto& chosen = visible ? visible : any;
    Found result{.cp = std::string{cp},
                 .version = chosen ? shown(chosen->first) : "",
                 .visible = visible.has_value(),
                 .installed = installed_best ? shown(*installed_best) : "",
                 .homepage = {},
                 .license = {},
                 .description = {}};
    if (chosen && chosen->second) {
        const auto& version = index.versions.at(*chosen->second);
        result.homepage = index.string(version.homepage);
        for (const auto id : index.ids_in(version.license)) {
            result.license +=
                std::format("{}{}", result.license.empty() ? "" : " ", index.string(id));
        }
        result.description = index.string(version.description);
    }
    return result;
}

std::vector<PackageVersion> Catalogue::versions(std::string_view cp) const {
    const auto& found = by_cp_.at(cp);
    const auto& index = index_.get();
    const auto& store = installed_.get();
    struct Entry {
        std::optional<Version> parsed;
        // Repository priority, or past every repository for an installed package alone.
        std::size_t order = 0;
        PackageVersion version;
    };
    std::vector<Entry> entries;
    for (const auto id : found.ebuilds) {
        const auto& ebuild = index.versions.at(id);
        const auto cpv = index.string(ebuild.cpv);
        const auto& masks = masks_.get();
        entries.push_back({.parsed = version_of(cp, cpv),
                           .order = ebuild.repository,
                           .version = {.version = std::string{cpv.substr(cp.size() + 1)},
                                       .slot = std::string{index.string(ebuild.slot)},
                                       .sub_slot = std::string{index.string(ebuild.sub_slot)},
                                       .repo = std::string{index.string(
                                           index.repositories.at(ebuild.repository).name)},
                                       .ebuild = true,
                                       .visible = masks.visible(id),
                                       .reasons = shown_reasons(index, masks, id),
                                       .installed = std::nullopt}});
    }
    const auto ebuilds = entries.size();
    for (const auto id : found.installed) {
        const auto& pkg = store.packages.at(id);
        const auto cpv = store.string(pkg.cpv);
        const auto repo = store.string(pkg.repo);
        const auto slot = store.string(pkg.slot);
        const auto sub_slot = store.string(pkg.sub_slot);
        const auto same = std::ranges::find_if(
            entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(ebuilds),
            [&](const Entry& entry) {
                return entry.version.repo == repo &&
                       cpv.substr(cp.size() + 1) == entry.version.version;
            });
        if (same != entries.begin() + static_cast<std::ptrdiff_t>(ebuilds)) {
            same->version.installed = id;
            continue;
        }
        entries.push_back({.parsed = version_of(cp, cpv),
                           .order = index.repositories.size(),
                           .version = {.version = std::string{cpv.substr(cp.size() + 1)},
                                       .slot = std::string{slot},
                                       .sub_slot = std::string{sub_slot.empty() ? slot : sub_slot},
                                       .repo = std::string{repo},
                                       .ebuild = false,
                                       .visible = installed_visible(id),
                                       .reasons = {},
                                       .installed = id}});
    }
    std::ranges::stable_sort(entries, [](const Entry& a, const Entry& b) {
        const int order = a.parsed && b.parsed ? vercmp(*a.parsed, *b.parsed) : 0;
        return order != 0 ? order < 0 : a.order < b.order;
    });
    std::vector<PackageVersion> versions;
    versions.reserve(entries.size());
    for (auto& entry : entries) {
        versions.push_back(std::move(entry.version));
    }
    return versions;
}

std::expected<std::vector<std::string>, std::string>
search_lines(const Store& installed, const Evaluated& evaluated, const RepositoryIndex& index,
             const VersionMasks& masks, std::span<const std::string> keys,
             const SearchOptions& options) {
    const Catalogue catalogue{installed, evaluated, index, masks};
    std::vector<std::string> lines;
    for (const auto& key : keys) {
        const auto cps = catalogue.search(key, options);
        if (!cps) {
            return std::unexpected(cps.error());
        }
        for (const auto cp : *cps) {
            const auto found = catalogue.found(cp);
            lines.push_back(std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", key, found.cp,
                                        found.version, found.visible ? "visible" : "masked",
                                        found.installed, found.homepage, found.license,
                                        found.description));
        }
    }
    return lines;
}

} // namespace egraph
