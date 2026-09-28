#pragma once

// Builds store bytes for tests, independently of the Python writer.

#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace egraph::test {

class Bytes {
  public:
    Bytes& varint(std::uint64_t value) {
        while (value > 0x7FU) {
            out_.push_back(std::byte{static_cast<std::uint8_t>((value & 0x7FU) | 0x80U)});
            value >>= 7U;
        }
        out_.push_back(std::byte{static_cast<std::uint8_t>(value)});
        return *this;
    }

    Bytes& text(std::string_view value) {
        varint(value.size());
        for (const char c : value) {
            out_.push_back(static_cast<std::byte>(c));
        }
        return *this;
    }

    Bytes& varints(std::initializer_list<std::uint64_t> values) {
        for (const auto value : values) {
            varint(value);
        }
        return *this;
    }

    // A count, then the values.
    Bytes& list(std::initializer_list<std::uint64_t> values) {
        varint(values.size());
        return varints(values);
    }

    Bytes& fixed(std::uint64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            out_.push_back(std::byte{static_cast<std::uint8_t>(value >> (8U * i))});
        }
        return *this;
    }

    Bytes& raw(const std::vector<std::byte>& bytes) {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
        return *this;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return out_; }

  private:
    std::vector<std::byte> out_;
};

struct Section {
    std::uint64_t id = 0;
    std::vector<std::byte> bytes;
};

inline std::vector<std::byte> assemble(const std::vector<Section>& sections,
                                       std::uint32_t version = store_format_version) {
    Bytes out;
    for (const char c : std::string_view{"EGRAPH"}) {
        out.fixed(static_cast<std::uint8_t>(c), 1);
    }
    out.fixed(0, 2).fixed(version, 4).fixed(sections.size(), 4);
    std::uint64_t offset = 16 + (20 * sections.size());
    for (const auto& section : sections) {
        out.fixed(section.id, 4).fixed(offset, 8).fixed(section.bytes.size(), 8);
        offset += section.bytes.size();
    }
    for (const auto& section : sections) {
        out.raw(section.bytes);
    }
    return out.bytes();
}

// Two packages, app-misc/a-1 and dev-libs/b-1:
//   a-1 RDEPEND: || ( dev-libs/b dev-libs/missing ) !app-misc/old, with an RDEPEND error,
//       USE and IUSE "flag", requiring x86_64 libb.so.1 from b-1.
//   b-1 provides x86_64 libb.so.1.
// Roots: app-misc/a in @selected, and dev-libs/missing, matching nothing, in @system.
inline constexpr std::initializer_list<std::string_view> sample_strings{
    "",          "app-misc/a-1",  "app-misc/a",       "0",        "test_repo", "8",
    "flag",      "dev-libs/b",    "dev-libs/b-1",     "RDEPEND",  "bad dep",   "x86_64",
    "libb.so.1", "!app-misc/old", "dev-libs/missing", "selected", "system"};

inline std::vector<Section> sample_sections() {
    Bytes meta;
    meta.text("0.0.0").text("3.0.0").text("/").varint(42);

    Bytes inputs;
    inputs.varint(1).text("/var/db/pkg/app-misc").varint(1).varint(5).varint(0);

    Bytes strings;
    strings.varint(sample_strings.size());
    for (const auto value : sample_strings) {
        strings.text(value);
    }

    Bytes packages;
    packages.varint(2);
    // a-1: cpv, cp, slot, sub-slot, repo, EAPI, IUSE_EFFECTIVE, USE, IUSE, errors.
    packages.varints({1, 2, 3, 3, 4, 5, 1});
    packages.list({6}).list({6}).varint(1).varint(9).varint(10);
    // BDEPEND, DEPEND, IDEPEND, PDEPEND are empty; RDEPEND has four nodes.
    packages.varint(0).varint(0).varint(0).varint(0).varint(4);
    packages.varint(1).varint(0).varint(0).list({});
    packages.varint(0).varint(1).varint(7).list({1});
    packages.varint(0).varint(1).varint(14).list({});
    packages.varint(3).varint(0).varint(13).list({});
    // Provides nothing; requires x86_64 libb.so.1, provided by package 1.
    packages.varint(0).varint(1).varint(11).varint(12).list({1});
    // b-1.
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    packages.varint(1).varint(11).varint(12).varint(0);

    Bytes roots;
    roots.varint(2).varint(15).varint(2).list({0});
    roots.varint(16).varint(14).list({});

    Bytes profile;
    profile.varint(2).text("amd64").text("elibc_glibc");
    profile.varint(1).text("build");
    profile.varint(1).text("elibc_");

    return {{.id = 1, .bytes = meta.bytes()},    {.id = 2, .bytes = inputs.bytes()},
            {.id = 3, .bytes = strings.bytes()}, {.id = 4, .bytes = packages.bytes()},
            {.id = 5, .bytes = roots.bytes()},   {.id = 6, .bytes = profile.bytes()}};
}

// The sample with one section replaced.
inline std::vector<std::byte> with_section(std::uint64_t id, const Bytes& bytes) {
    auto sections = sample_sections();
    for (auto& section : sections) {
        if (section.id == id) {
            section.bytes = bytes.bytes();
        }
    }
    return assemble(sections);
}

// The sample with its packages replaced by app-misc/a-1 alone, whose RDEPEND is count nodes
// written by nodes as (type, parent, atom, matches).
inline std::vector<std::byte> with_rdepend(std::uint64_t count,
                                           const std::function<void(Bytes&)>& nodes) {
    Bytes packages;
    packages.varint(1).varints({1, 2, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(count);
    nodes(packages);
    packages.varint(0).varint(0);
    return with_section(4, packages);
}

// The sample with more strings after sample_strings, and one more section replaced.
inline std::vector<std::byte> with_strings(std::initializer_list<std::string_view> extra,
                                           std::uint64_t id, const Bytes& bytes) {
    Bytes strings;
    strings.varint(sample_strings.size() + extra.size());
    for (const auto value : sample_strings) {
        strings.text(value);
    }
    for (const auto value : extra) {
        strings.text(value);
    }
    auto sections = sample_sections();
    for (auto& section : sections) {
        if (section.id == 3) {
            section.bytes = strings.bytes();
        } else if (section.id == id) {
            section.bytes = bytes.bytes();
        }
    }
    return assemble(sections);
}

// a-1 and b-1, where a-1 has only BDEPEND (build_time) or RDEPEND: || ( >=dev-libs/b-2
// dev-libs/missing ) with b-1 installed, string 17 being the first atom.
inline std::vector<std::byte> newer_wanted(bool build_time) {
    Bytes packages;
    packages.varint(2);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    const auto group = [](Bytes& out) {
        out.varint(3);
        out.varints({1, 0, 0}).list({});
        out.varints({0, 1, 17}).list({});
        out.varints({0, 1, 14}).list({});
    };
    if (build_time) {
        group(packages);
        packages.varint(0).varint(0).varint(0).varint(0);
    } else {
        packages.varint(0).varint(0).varint(0).varint(0);
        group(packages);
    }
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    return with_strings({">=dev-libs/b-2"}, 4, packages);
}

// The sample without inputs, which is therefore always fresh.
inline std::vector<std::byte> fresh_sample() {
    return with_section(2, Bytes{}.varint(0));
}

} // namespace egraph::test
