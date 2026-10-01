#pragma once

#include "evaluated.hpp"
#include "plan.hpp"
#include "store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace egraph {

// Where USE changes are written under the configuration root: package.use/zz-autounmask when
// package.use is a directory, so that it sorts last and wins, else package.use itself.
[[nodiscard]] std::filesystem::path package_use_path(const std::filesystem::path& config_root);

// The plan's USE changes as package.use text: each change's "# required by" comments, then its
// line (package_use_line), every line ending in a newline.
[[nodiscard]] std::string package_use_text(const Store& store, const Evaluated& evaluated,
                                           const Plan& plan);

// Appends text to the file at path, creating it, on a line of its own; the error when it could
// not.
[[nodiscard]] std::optional<std::string> append_to_file(const std::filesystem::path& path,
                                                        std::string_view text);

} // namespace egraph
