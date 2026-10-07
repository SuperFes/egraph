#pragma once

// What needs the user once emerge has run, as egraph-build --notices writes it, and the GLSAs
// affecting the installed packages.

#include "evaluated.hpp"
#include "glsa.hpp"
#include "history.hpp"
#include "repository.hpp"
#include "store.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

struct Notices {
    // A ._cfg file waiting to replace a protected configuration file.
    struct ConfigUpdate {
        std::string file;
        std::string update;
    };
    // An unread news item of a repository.
    struct News {
        std::string repo;
        std::string item;
        std::string title;
    };
    // A library emerge kept after its package stopped providing it, for what still uses it.
    struct Preserved {
        std::string path;
        std::string package;
        // The installed packages using it, by cpv.
        std::vector<std::string> consumers;
    };
    std::vector<ConfigUpdate> config;
    std::vector<News> news;
    // None when the preserved libraries' registry cannot be read (only root and the portage
    // group may).
    std::optional<std::vector<Preserved>> preserved;
    // @preserved-rebuild's atoms, as emerge loads the set; none when the libraries' consumers
    // cannot be found.
    std::optional<std::vector<std::string>> rebuild;
    // The rest come from the stores rather than egraph-build.
    std::vector<AffectedAdvisory> advisories;
    // A repository synced longer ago than the settings allow.
    struct Stale {
        std::string name;
        Seconds synced;
    };
    std::vector<Stale> stale;
    // An installed package masked now, with why as emerge words it.
    struct Masked {
        std::string cpv;
        std::vector<std::string> reasons;
    };
    std::vector<Masked> masked;
    // A soname an installed package needs that nothing installed provides.
    struct Missing {
        std::string cpv;
        std::string category;
        std::string soname;
    };
    std::vector<Missing> missing;
};

// The repositories whose timestamp.chk is older than days before now; none when days is 0, nor
// for one without the file.
[[nodiscard]] std::vector<Notices::Stale> stale_repositories(const RepositoryIndex& index,
                                                             Seconds now, int days);

// The installed packages the evaluated store finds masked, under --dynamic-deps=y or =n.
[[nodiscard]] std::vector<Notices::Masked>
masked_installed(const Store& store, const Evaluated& evaluated, bool dynamic_deps);

// Each soname an installed package requires that no installed package provides, by cpv.
[[nodiscard]] std::vector<Notices::Missing> missing_sonames(const Store& store);

enum class NoticeKind : std::uint8_t { glsa, news, config, preserved, stale, masked, missing };

// "glsa", "news", "config", "preserved", "stale", "masked", "missing".
[[nodiscard]] std::string_view notice_kind_name(NoticeKind kind);
[[nodiscard]] std::optional<NoticeKind> notice_kind(std::string_view name);

// One thing to act on.
struct Notice {
    NoticeKind kind = NoticeKind::news;
    // Stable while it lasts: "glsa:202609-03", "news:gentoo/2026-09-01-x", "config",
    // "preserved", "stale:gentoo", "masked:cat/pkg-1", "missing:cat/pkg-1".
    std::string key;
    std::string title;
    std::vector<std::string> detail;
    // Changes when what it says does (a GLSA revised, another version affected, a new sync),
    // which brings a dismissed notice back.
    std::string fingerprint;
    // When it was first noticed.
    Seconds since{};
    auto operator<=>(const Notice&) const = default;
};

// The notices as one list: a GLSA, a news item, a stale repository, a masked package, and the
// missing sonames of a package each one; the configuration updates and the preserved libraries
// one each, a single command dealing with all. Ages count to now; each is since now.
[[nodiscard]] std::vector<Notice> notice_list(const Notices& notices, Seconds now);

// since carried over from the notice of the same key in previous, where there is one.
void carry_since(std::vector<Notice>& notices, std::span<const Notice> previous);

// The notices whose keys previous lacks.
[[nodiscard]] std::vector<Notice> new_notices(std::span<const Notice> notices,
                                              std::span<const Notice> previous);

// notices.missing less the sonames a preserved library still provides, by its file name: those
// are the preserved libraries' notices.
void drop_preserved(Notices& notices);

[[nodiscard]] std::expected<Notices, std::string> parse_notices(std::string_view text);

// "file<TAB>config<TAB>update" for each configuration update,
// "item<TAB>news<TAB>repo<TAB>title" for each unread news item,
// "path<TAB>preserved<TAB>package<TAB>consumers" (space-separated) for each preserved library,
// then "atom<TAB>rebuild" for each atom of @preserved-rebuild, and
// "id<TAB>glsa<TAB>title<TAB>cpv<TAB>fixed" (space-separated) for each package a GLSA affects,
// "repo<TAB>stale<TAB>synced" (seconds) for each stale repository,
// "cpv<TAB>masked<TAB>reasons" (comma-separated) for each masked installed package, and
// "cpv<TAB>missing<TAB>category<TAB>soname" for each soname nothing provides.
[[nodiscard]] std::vector<std::string> notice_lines(const Notices& notices);

} // namespace egraph
