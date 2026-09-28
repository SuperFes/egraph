#include "human.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ostream>
#include <vector>

namespace egraph {

namespace {

using Fields = std::vector<std::string_view>;

Fields split(std::string_view record) {
    Fields fields;
    while (true) {
        const auto tab = record.find('\t');
        fields.push_back(record.substr(0, tab));
        if (tab == std::string_view::npos) {
            return fields;
        }
        record.remove_prefix(tab + 1);
    }
}

std::vector<Fields> split_all(std::span<const std::string> records) {
    std::vector<Fields> all;
    all.reserve(records.size());
    for (const auto& record : records) {
        all.push_back(split(record));
    }
    return all;
}

std::string padded(std::string_view text, std::size_t width) {
    std::string out{text};
    out.resize(std::max(width, text.size()), ' ');
    return out;
}

// Runtime kinds first, as depclean reads them.
constexpr std::array<std::string_view, 5> kind_order{"RDEPEND", "IDEPEND", "PDEPEND", "DEPEND",
                                                     "BDEPEND"};

std::string count(std::size_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

} // namespace

std::string Painter::operator()(std::string_view text, Tone tone) const {
    if (!color_ || text.empty()) {
        return std::string{text};
    }
    std::string_view code;
    switch (tone) {
    case Tone::heading:
    case Tone::package:
        code = "1";
        break;
    case Tone::kind:
        code = "36";
        break;
    case Tone::atom:
    case Tone::note:
        code = "2";
        break;
    case Tone::choice:
        code = "33";
        break;
    case Tone::root:
        code = "1;32";
        break;
    case Tone::bad:
        code = "31";
        break;
    }
    return std::format("\x1b[{}m{}\x1b[0m", code, text);
}

void human_edges(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> subjects, bool reverse, const Painter& paint) {
    const auto rows = split_all(records);
    const std::size_t self = reverse ? 3 : 0;
    const std::size_t other = reverse ? 0 : 3;
    bool first = true;
    for (const auto& subject : subjects) {
        out << (first ? "" : "\n") << paint(subject, Tone::package) << '\n';
        first = false;
        // One line per package and atom, with every kind it appears under.
        struct Line {
            std::string_view package;
            std::string_view atom;
            bool choice = false;
            std::string kinds;
        };
        std::vector<Line> lines;
        for (const auto kind : kind_order) {
            for (const auto& row : rows) {
                if (row.at(self) != subject || row.at(1) != kind) {
                    continue;
                }
                const bool choice = row.size() > 4;
                const auto found = std::ranges::find_if(lines, [&](const Line& line) {
                    return line.package == row.at(other) && line.atom == row.at(2) &&
                           line.choice == choice;
                });
                if (found == lines.end()) {
                    lines.push_back({.package = row.at(other),
                                     .atom = row.at(2),
                                     .choice = choice,
                                     .kinds = std::string{kind}});
                } else {
                    found->kinds += ' ';
                    found->kinds += kind;
                }
            }
        }
        if (lines.empty()) {
            out << "  "
                << paint(reverse ? "nothing installed depends on it" : "no dependencies",
                         Tone::note)
                << '\n';
            continue;
        }
        std::ranges::stable_sort(lines, {}, &Line::package);
        std::size_t package_width = 0;
        std::size_t kinds_width = 0;
        for (const auto& line : lines) {
            package_width = std::max(package_width, line.package.size());
            kinds_width = std::max(kinds_width, line.kinds.size());
        }
        for (const auto& line : lines) {
            out << "  " << paint(padded(line.package, package_width), Tone::package) << "  "
                << paint(padded(line.kinds, kinds_width), Tone::kind) << "  "
                << paint(line.atom, Tone::atom);
            if (line.choice) {
                out << "  " << paint("any-of", Tone::choice);
            }
            out << '\n';
        }
    }
}

void human_path(std::ostream& out, std::span<const std::string> records, const Painter& paint) {
    const auto rows = split_all(records);
    if (rows.empty()) {
        return;
    }
    const auto& root = rows.front();
    out << paint(root.at(0), Tone::root) << "  " << paint(root.at(1), Tone::atom) << '\n';
    // Each package one level deeper than the one depending on it; "└─ " is 3 columns.
    const auto depth_of = [](std::size_t depth) { return (3 * depth) + 3; };
    std::size_t width = depth_of(0) + root.at(2).size();
    for (std::size_t i = 1; i < rows.size(); ++i) {
        width = std::max(width, depth_of(i) + rows.at(i).at(3).size());
    }
    const auto line = [&](std::size_t depth, std::string_view package) {
        const std::string indent(3 * depth, ' ');
        out << indent << "└─ " << paint(package, Tone::package);
    };
    line(0, root.at(2));
    out << '\n';
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& edge = rows.at(i);
        line(i, edge.at(3));
        out << std::string(width - depth_of(i) - edge.at(3).size(), ' ') << "  "
            << paint(edge.at(1), Tone::kind) << ' ' << paint(edge.at(2), Tone::atom);
        if (edge.size() > 4) {
            out << "  " << paint("any-of", Tone::choice);
        }
        out << '\n';
    }
}

void human_orphans(std::ostream& out, std::span<const std::string> records, const Painter& paint) {
    for (const auto& cpv : records) {
        out << paint(cpv, Tone::package) << '\n';
    }
    if (records.empty()) {
        out << paint("Nothing to remove.", Tone::note) << '\n';
        return;
    }
    out << '\n'
        << paint(count(records.size(), "package", "packages") + " depclean would remove",
                 Tone::note)
        << '\n';
}

void human_broken(std::ostream& out, std::span<const std::string> records, const Painter& paint) {
    const auto rows = split_all(records);
    std::size_t packages = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        if (i == 0 || rows.at(i - 1).at(0) != row.at(0)) {
            out << (i == 0 ? "" : "\n") << paint(row.at(0), Tone::package) << '\n';
            ++packages;
        }
        out << "  " << paint(padded(row.at(1), 7), Tone::kind) << "  "
            << paint(row.at(2), Tone::bad) << '\n';
    }
    if (rows.empty()) {
        out << paint("Every dependency is satisfied.", Tone::note) << '\n';
        return;
    }
    out << '\n'
        << paint(
               std::format("{} in {}",
                           count(rows.size(), "unsatisfied dependency", "unsatisfied dependencies"),
                           count(packages, "package", "packages")),
               Tone::note)
        << '\n';
}

void human_soname(std::ostream& out, std::span<const std::string> records, std::string_view soname,
                  bool providers, const Painter& paint) {
    if (records.empty()) {
        out << paint(
                   std::format("Nothing installed {} {}.", providers ? "provides" : "uses", soname),
                   Tone::note)
            << '\n';
        return;
    }
    out << paint(soname, Tone::heading) << (providers ? " is provided by" : " is used by") << '\n';
    auto rows = split_all(records);
    std::ranges::stable_sort(rows, {}, [](const Fields& row) { return row.at(1); });
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        if (i == 0 || rows.at(i - 1).at(1) != row.at(1)) {
            out << "  "
                << paint(row.at(1).empty() ? "(no multilib category)" : row.at(1), Tone::kind)
                << '\n';
        }
        out << "    " << paint(row.at(0), Tone::package) << '\n';
    }
}

void human_match(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> atoms, const Painter& paint) {
    const auto rows = split_all(records);
    bool first = true;
    for (const auto& atom : atoms) {
        out << (first ? "" : "\n") << paint(atom, Tone::heading) << '\n';
        first = false;
        bool any = false;
        for (const auto& row : rows) {
            if (row.at(0) == atom) {
                out << "  " << paint(row.at(1), Tone::package) << '\n';
                any = true;
            }
        }
        if (!any) {
            out << "  " << paint("no installed package", Tone::note) << '\n';
        }
    }
}

} // namespace egraph
