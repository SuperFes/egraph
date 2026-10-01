#pragma once

// What needs the user once emerge has run, as egraph-build --notices writes it.

#include <expected>
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
    std::vector<ConfigUpdate> config;
    std::vector<News> news;
};

[[nodiscard]] std::expected<Notices, std::string> parse_notices(std::string_view text);

// "file<TAB>config<TAB>update" for each configuration update, then
// "item<TAB>news<TAB>repo<TAB>title" for each unread news item.
[[nodiscard]] std::vector<std::string> notice_lines(const Notices& notices);

} // namespace egraph
