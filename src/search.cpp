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
std::string shown(const Version& version) {
    if (version.revision == "0") {
        return version.base;
    }
    return version.text;
}

// The packages search_lines reports on: the repository index's versions and the installed
// ones, by cp.
class Catalogue {
  public:
    Catalogue(const Store& installed, const Evaluated& evaluated, const RepositoryIndex& index,
              const VersionMasks& masks)
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

    [[nodiscard]] std::span<const std::string_view> cps() const EGRAPH_LIFETIMEBOUND {
        return cps_;
    }

    // The DESCRIPTION emerge --search -S matches, _first_cp's through its IndexedPortdb: of the
    // repositories with a pkg_desc_index, read lowest priority first, the first holding cp gives
    // its lowest version, whose description is that of the last of them listing it, egencache's
    // description of its highest version. Without one, the lowest version's own, from the
    // repository of highest priority holding it. Empty for a cp only installed.
    [[nodiscard]] std::string_view description(std::string_view cp) const EGRAPH_LIFETIMEBOUND {
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
        // The indexed repository of highest priority listing that version wrote it last.
        std::optional<std::uint32_t> last;
        for (const auto id : ebuilds) {
            const auto parsed = version(id);
            if (indexed(id) && parsed && vercmp(*parsed, lowest->first) == 0 &&
                (!last || repository(id) < *last)) {
                last = repository(id);
            }
        }
        const auto highest =
            extreme([&](std::uint32_t id) { return repository(id) == *last; }, true);
        return index.string(index.versions.at(highest->second).description);
    }

    // The line for cp under key.
    [[nodiscard]] std::string line(std::string_view key, std::string_view cp) const {
        const auto& found = by_cp_.at(cp);
        const auto& index = index_.get();
        const auto& store = installed_.get();
        // Best visible, else best of all: (version, ebuild id if one holds it).
        std::optional<std::pair<Version, std::optional<std::uint32_t>>> visible;
        std::optional<std::pair<Version, std::optional<std::uint32_t>>> any;
        const auto consider = [](auto& best, const Version& version,
                                 std::optional<std::uint32_t> ebuild, const auto& earlier_than) {
            if (!best || vercmp(version, best->first) > 0) {
                best = {version, ebuild};
            } else if (vercmp(version, best->first) == 0 && ebuild &&
                       (!best->second || earlier_than(*ebuild, *best->second))) {
                best->second = ebuild;
            }
        };
        const auto earlier_than = [this](std::uint32_t a, std::uint32_t b) {
            return earlier(a, b);
        };
        for (const auto id : found.ebuilds) {
            const auto version = version_of(cp, index.string(index.versions.at(id).cpv));
            if (!version) {
                continue;
            }
            consider(any, *version, id, earlier_than);
            if (masks_.get().visible(id)) {
                consider(visible, *version, id, earlier_than);
            }
        }
        std::optional<Version> installed_best;
        for (const auto id : found.installed) {
            const auto version = version_of(cp, store.string(store.packages.at(id).cpv));
            if (!version) {
                continue;
            }
            consider(any, *version, std::nullopt, earlier_than);
            if (evaluated_.get().packages.at(id).vdb_hidden == Hidden::visible) {
                consider(visible, *version, std::nullopt, earlier_than);
            }
            if (!installed_best || vercmp(*version, *installed_best) > 0) {
                installed_best = *version;
            }
        }
        const auto& chosen = visible ? visible : any;
        std::string homepage;
        std::string license;
        std::string description;
        if (chosen && chosen->second) {
            const auto& version = index.versions.at(*chosen->second);
            homepage = index.string(version.homepage);
            for (const auto id : index.ids_in(version.license)) {
                license += std::string{license.empty() ? "" : " "} + std::string{index.string(id)};
            }
            description = index.string(version.description);
        }
        return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", key, cp,
                           chosen ? shown(chosen->first) : "", visible ? "visible" : "masked",
                           installed_best ? shown(*installed_best) : "", homepage, license,
                           description);
    }

  private:
    struct Versions {
        std::vector<std::uint32_t> ebuilds;
        std::vector<std::uint32_t> installed;
    };

    // Of two ebuilds of one version, whether a's repository has the higher priority.
    [[nodiscard]] bool earlier(std::uint32_t a, std::uint32_t b) const {
        return index_.get().versions.at(a).repository < index_.get().versions.at(b).repository;
    }

    std::reference_wrapper<const Store> installed_;
    std::reference_wrapper<const Evaluated> evaluated_;
    std::reference_wrapper<const RepositoryIndex> index_;
    std::reference_wrapper<const VersionMasks> masks_;
    std::map<std::string_view, Versions, std::less<>> by_cp_;
    std::vector<std::string_view> cps_;
};

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

std::expected<std::vector<std::string>, std::string>
search_lines(const Store& installed, const Evaluated& evaluated, const RepositoryIndex& index,
             const VersionMasks& masks, std::span<const std::string> keys,
             const SearchOptions& options) {
    const Catalogue catalogue{installed, evaluated, index, masks};
    std::vector<std::string> lines;
    for (const auto& key : keys) {
        const auto found = search_cps(
            catalogue.cps(), [&](std::string_view cp) { return catalogue.description(cp); }, key,
            options);
        if (!found) {
            return std::unexpected(found.error());
        }
        for (const auto cp : *found) {
            lines.push_back(catalogue.line(key, cp));
        }
    }
    return lines;
}

} // namespace egraph
