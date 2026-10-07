#pragma once

// The system store's history: the generations a refresh replaced, in ${EPREFIX}/var/lib/egraph.

#include "store.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// egraph's settings, from ${PORTAGE_CONFIGROOT}/etc/egraph/egraph.conf.
struct Settings {
    // How many days of generations to keep; 0 keeps none.
    int history_days = 90;
};

// "key = value" lines, blank lines and "#" comments skipped. An error names the line of an
// unknown key or a value that does not parse.
[[nodiscard]] std::expected<Settings, std::string> parse_settings(std::string_view text);
[[nodiscard]] std::filesystem::path settings_path(const std::filesystem::path& config_root);
// The settings at path; the defaults when there is none.
[[nodiscard]] std::expected<Settings, std::string> read_settings(const std::filesystem::path& path);

// ${ROOT}${EPREFIX}/var/lib/egraph
[[nodiscard]] std::filesystem::path history_directory(const std::filesystem::path& root,
                                                      const std::filesystem::path& eprefix);

using Seconds = std::chrono::sys_seconds;

// A generation's file name from the time its store was built, in UTC:
// installed-20261006T162600Z.egraph; and back, none for any other name.
[[nodiscard]] std::string generation_name(Seconds built);
[[nodiscard]] std::optional<Seconds> generation_time(std::string_view name);

// Of the generations built at these times, those to delete at now: kept are all from the last
// day, then the newest of each UTC day up to days old.
[[nodiscard]] std::vector<Seconds> thinned(std::span<const Seconds> generations, Seconds now,
                                           int days);

// Whether after differs from before in its installed packages or its root sets.
[[nodiscard]] bool history_changed(const Store& before, const Store& after);

} // namespace egraph
