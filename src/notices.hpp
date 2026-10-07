#pragma once

// What needs the user once emerge has run, as egraph-build --notices writes it, and the GLSAs
// affecting the installed packages.

#include "glsa.hpp"

#include <expected>
#include <optional>
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
    // Matched against the stores rather than read by egraph-build.
    std::vector<AffectedAdvisory> advisories;
};

[[nodiscard]] std::expected<Notices, std::string> parse_notices(std::string_view text);

// "file<TAB>config<TAB>update" for each configuration update,
// "item<TAB>news<TAB>repo<TAB>title" for each unread news item,
// "path<TAB>preserved<TAB>package<TAB>consumers" (space-separated) for each preserved library,
// then "atom<TAB>rebuild" for each atom of @preserved-rebuild, and
// "id<TAB>glsa<TAB>title<TAB>cpv<TAB>fixed" (space-separated) for each package a GLSA affects.
[[nodiscard]] std::vector<std::string> notice_lines(const Notices& notices);

} // namespace egraph
