#include "cgroup.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace egraph::cgroup {

namespace {

// What each leaf needs for cpu.stat, memory.peak and io.stat.
constexpr std::array<std::string_view, 3> controllers{"cpu", "io", "memory"};

template <class T> std::optional<T> number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.begin(), text.end(), value);
    if (error != std::errc{} || end != text.end()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::set<std::string> words_of(const std::filesystem::path& path) {
    std::set<std::string> words;
    if (const auto text = read_text(path)) {
        std::istringstream in{*text};
        for (std::string word; in >> word;) {
            words.insert(word);
        }
    }
    return words;
}

// Gives dir's children the controllers it has of those the leaves need, unless it does already.
void enable_controllers(const std::filesystem::path& dir) {
    const auto available = words_of(dir / "cgroup.controllers");
    const auto enabled = words_of(dir / "cgroup.subtree_control");
    std::string wanted;
    bool missing = false;
    for (const auto controller : controllers) {
        if (available.contains(std::string{controller})) {
            wanted += std::format("{}+{}", wanted.empty() ? "" : " ", controller);
            missing = missing || !enabled.contains(std::string{controller});
        }
    }
    if (missing) {
        std::ofstream{dir / "cgroup.subtree_control"} << wanted;
    }
}

std::optional<std::uint64_t> read_number(const std::filesystem::path& path) {
    auto text = read_text(path);
    if (!text) {
        return std::nullopt;
    }
    while (!text->empty() && text->back() == '\n') {
        text->pop_back();
    }
    return number<std::uint64_t>(*text);
}

std::optional<std::uint64_t> stall_total(const std::filesystem::path& path) {
    const auto text = read_text(path);
    return text ? pressure::parse_psi_total(*text) : std::nullopt;
}

} // namespace

std::optional<std::uint64_t> parse_keyed(std::string_view text, std::string_view key) {
    for (const auto each : std::views::split(text, '\n')) {
        const std::string_view line{each.begin(), each.end()};
        const auto space = line.find(' ');
        if (space != std::string_view::npos && line.substr(0, space) == key) {
            return number<std::uint64_t>(line.substr(space + 1));
        }
    }
    return std::nullopt;
}

std::optional<Io> parse_io_stat(std::string_view text) {
    Io io;
    for (const auto each : std::views::split(text, '\n')) {
        const std::string_view line{each.begin(), each.end()};
        // "MAJ:MIN key=value ...": the device first.
        bool device = true;
        for (const auto part : std::views::split(line, ' ')) {
            const std::string_view field{part.begin(), part.end()};
            if (std::exchange(device, false) || field.empty()) {
                continue;
            }
            const auto equals = field.find('=');
            const auto key = field.substr(0, equals);
            if (equals == std::string_view::npos || (key != "rbytes" && key != "wbytes")) {
                continue;
            }
            const auto value = number<std::uint64_t>(field.substr(equals + 1));
            if (!value) {
                return std::nullopt;
            }
            (key == "rbytes" ? io.read_bytes : io.write_bytes) += *value;
        }
    }
    return io;
}

std::string leaf_name(std::string_view cpv) {
    std::string name{cpv};
    std::ranges::replace_if(
        name,
        [](char c) {
            const bool alnum =
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            return !alnum && c != '.' && c != '_' && c != '+' && c != '-';
        },
        '_');
    return name;
}

std::optional<Counters> read_counters(const std::filesystem::path& leaf) {
    std::error_code error;
    if (!std::filesystem::is_directory(leaf, error)) {
        return std::nullopt;
    }
    Counters counters;
    if (const auto cpu = read_text(leaf / "cpu.stat")) {
        counters.cpu_usec = parse_keyed(*cpu, "usage_usec");
    }
    counters.memory_peak = read_number(leaf / "memory.peak");
    if (const auto text = read_text(leaf / "io.stat")) {
        if (const auto io = parse_io_stat(*text)) {
            counters.read_bytes = io->read_bytes;
            counters.write_bytes = io->write_bytes;
        }
    }
    counters.stalls = {.cpu = stall_total(leaf / "cpu.pressure"),
                       .memory = stall_total(leaf / "memory.pressure"),
                       .io = stall_total(leaf / "io.pressure")};
    return counters;
}

void remove(const std::filesystem::path& cgroup) {
    std::error_code error;
    if (!std::filesystem::is_directory(cgroup, error)) {
        return;
    }
    // cgroup.kill needs Linux 5.14; without it, whatever is left keeps its cgroup.
    if (std::filesystem::exists(cgroup / "cgroup.kill", error)) {
        std::ofstream{cgroup / "cgroup.kill"} << "1";
    }
    std::vector<std::filesystem::path> children;
    for (const auto& entry : std::filesystem::directory_iterator{cgroup, error}) {
        if (entry.is_directory(error)) {
            children.push_back(entry.path());
        }
    }
    for (const auto& child : children) {
        cgroup::remove(child);
    }
    // A cgroup's interface files go with it: rmdir, never remove_all.
    std::filesystem::remove(cgroup, error);
}

std::expected<Hierarchy, std::string> Hierarchy::create(const std::filesystem::path& cgroupfs,
                                                        std::int64_t pid,
                                                        const std::filesystem::path& proc) {
    std::error_code error;
    if (!std::filesystem::exists(cgroupfs / "cgroup.controllers", error)) {
        return std::unexpected(std::format("{} is not cgroup v2", cgroupfs.string()));
    }
    const auto base = cgroupfs / "egraph";
    std::filesystem::create_directory(base, error);
    if (error) {
        return std::unexpected(std::format("{}: {}", base.string(), error.message()));
    }
    enable_controllers(cgroupfs);
    enable_controllers(base);
    // Runs killed before they could remove theirs.
    std::vector<std::filesystem::path> stale;
    for (const auto& entry : std::filesystem::directory_iterator{base, error}) {
        const auto name = entry.path().filename().string();
        const auto other =
            name.starts_with("exec-") ? number<std::int64_t>(name.substr(5)) : std::nullopt;
        if (other && *other != pid && !std::filesystem::exists(proc / std::to_string(*other))) {
            stale.push_back(entry.path());
        }
    }
    for (const auto& path : stale) {
        cgroup::remove(path);
    }
    const auto root = base / std::format("exec-{}", pid);
    std::filesystem::create_directory(root, error);
    if (error) {
        return std::unexpected(std::format("{}: {}", root.string(), error.message()));
    }
    enable_controllers(root);
    return Hierarchy{root};
}

Hierarchy::Hierarchy(Hierarchy&& other) noexcept : root_{std::exchange(other.root_, {})} {}

Hierarchy& Hierarchy::operator=(Hierarchy&& other) noexcept {
    if (this != &other) {
        if (!root_.empty()) {
            cgroup::remove(root_);
        }
        root_ = std::exchange(other.root_, {});
    }
    return *this;
}

Hierarchy::~Hierarchy() {
    if (!root_.empty()) {
        cgroup::remove(root_);
    }
}

std::filesystem::path Hierarchy::leaf(std::string_view cpv) const {
    return root_ / leaf_name(cpv);
}

} // namespace egraph::cgroup
