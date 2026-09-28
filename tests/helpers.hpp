#pragma once

// Filesystem fixtures shared by the tests.

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace egraph::test {

class TempDir {
  public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                std::format("egraph-test-{}-{}", ::getpid(), counter++)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    static inline int counter = 0;
    std::filesystem::path path_;
};

inline void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary);
    for (const auto byte : bytes) {
        out.put(static_cast<char>(byte));
    }
}

inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A stand-in for egraph-build in dir: records its arguments in dir/args, installs `prepared` at
// the --store path it was given, and exits with status.
inline std::filesystem::path fake_builder(const std::filesystem::path& dir,
                                          const std::vector<std::byte>& prepared, int status) {
    const auto script = dir / "egraph-build";
    write_text(script, std::format("#!/bin/sh\necho \"$@\" > '{}'\nmkdir -p \"$(dirname \"$3\")\"\n"
                                   "cp '{}' \"$3\"\nexit {}\n",
                                   (dir / "args").string(), (dir / "prepared").string(), status));
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    write_bytes(dir / "prepared", prepared);
    return script;
}

} // namespace egraph::test
