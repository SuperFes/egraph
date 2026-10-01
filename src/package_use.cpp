#include "package_use.hpp"

#include <format>
#include <fstream>
#include <system_error>

namespace egraph {

std::filesystem::path package_use_path(const std::filesystem::path& config_root) {
    auto path = config_root / "etc/portage/package.use";
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return path / "zz-autounmask";
    }
    return path;
}

std::string package_use_text(const Store& store, const Evaluated& original, const Plan& plan) {
    const auto& evaluated = plan.evaluated_or(original);
    std::string text;
    for (const auto& needed : plan.use_changes) {
        for (const auto& link : required_by(store, evaluated, plan, needed)) {
            text += std::format("# required by {}\n", link);
        }
        text += package_use_line(store, evaluated, needed.change) + '\n';
    }
    return text;
}

std::optional<std::string> append_to_file(const std::filesystem::path& path,
                                          std::string_view text) {
    std::error_code error;
    bool newline = true;
    if (const auto size = std::filesystem::file_size(path, error); !error && size > 0) {
        std::ifstream in(path, std::ios::binary);
        in.seekg(-1, std::ios::end);
        char last = '\n';
        newline = in.get(last) && last == '\n';
    }
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        return std::format("cannot write {}", path.string());
    }
    out << (newline ? "" : "\n") << text;
    out.flush();
    if (!out) {
        return std::format("cannot write {}", path.string());
    }
    return std::nullopt;
}

} // namespace egraph
