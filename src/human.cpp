#include "human.hpp"
#include "required_use.hpp"
#include "status.hpp"

#include "version.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace egraph {

namespace {

// Catppuccin Mocha, with the nearest xterm-256 colour for terminals without truecolor.
ToneStyle mocha(Tone tone) {
    switch (tone) {
    case Tone::heading:
    case Tone::name:
        return {.bold = true, .red = 205, .green = 214, .blue = 244, .xterm = 189};
    case Tone::category:
        return {.red = 137, .green = 180, .blue = 250, .xterm = 111};
    case Tone::version:
        return {.red = 166, .green = 227, .blue = 161, .xterm = 150};
    case Tone::op:
        return {.red = 243, .green = 139, .blue = 168, .xterm = 211};
    case Tone::slot:
        return {.red = 249, .green = 226, .blue = 175, .xterm = 223};
    case Tone::use:
        return {.red = 203, .green = 166, .blue = 247, .xterm = 183};
    case Tone::repo:
        return {.red = 250, .green = 179, .blue = 135, .xterm = 216};
    case Tone::runtime:
    case Tone::good:
        return {.bold = true, .red = 166, .green = 227, .blue = 161, .xterm = 150};
    case Tone::install:
        return {.bold = true, .red = 148, .green = 226, .blue = 213, .xterm = 116};
    case Tone::post:
        return {.bold = true, .red = 116, .green = 199, .blue = 236, .xterm = 117};
    case Tone::build:
        return {.bold = true, .red = 249, .green = 226, .blue = 175, .xterm = 223};
    case Tone::host:
    case Tone::count:
        return {.bold = true, .red = 250, .green = 179, .blue = 135, .xterm = 216};
    case Tone::choice:
        return {.red = 245, .green = 194, .blue = 231, .xterm = 218};
    case Tone::root:
        return {.bold = true, .red = 249, .green = 226, .blue = 175, .xterm = 222};
    case Tone::bad:
        return {.bold = true, .red = 243, .green = 139, .blue = 168, .xterm = 204};
    case Tone::note:
        return {.italic = true, .red = 127, .green = 132, .blue = 156, .xterm = 245};
    }
    return {};
}

constexpr Glyphs nerd_glyphs{
    .package = "",
    .selected = "",
    .system = "",
    .profile = "",
    .set = "",
    .orphan = "",
    .broken = "",
    .soname = "",
    .search = "",
    .good = "",
    .choice = "",
    .branch = "╰─",
    .absent = "·",
    .tee = "├─",
    .rail = "│ ",
    .folded = "▸",
    .unfolded = "▾",
    .cycle = "↻",
    .instead = "→",
    .upgrade = "\uF0AA",
    .downgrade = "\uF0AB",
    .rebuild = "\uF021",
    .added = "\uF0FE",
    .held = "\uF023",
    .frame = {.top_left = "╭",
              .top_right = "╮",
              .bottom_left = "╰",
              .bottom_right = "╯",
              .across = "─",
              .down = "│"},
    .build = "\uF013",
    .binary = "\uF1B2",
    .merge = "\uF019",
    .waiting = "\uF254",
    .queued = "\uF017",
    .spinner = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏",
    .bar_full = "█",
    .bar_empty = "░",
    .bar_eighths = "▏▎▍▌▋▊▉",
    .spark = "▁▂▃▄▅▆▇█",
    .move = "↑↓",
    .enter = "⏎",
    .pages = "←→",
    .trail = "\uE0B1",
    .cursor = "▌",
    .installed = "●",
    .pick = "★",
    .superscripts = "⁰¹²³⁴⁵⁶⁷⁸⁹",
};

constexpr Glyphs unicode_glyphs{
    .package = "◆",
    .selected = "★",
    .system = "◎",
    .profile = "◉",
    .set = "▤",
    .orphan = "✗",
    .broken = "△",
    .soname = "⌁",
    .search = "⌕",
    .good = "✓",
    .choice = "∨",
    .branch = "╰─",
    .absent = "·",
    .tee = "├─",
    .rail = "│ ",
    .folded = "▸",
    .unfolded = "▾",
    .cycle = "↻",
    .instead = "→",
    .upgrade = "↑",
    .downgrade = "↓",
    .rebuild = "↺",
    .added = "⊕",
    .held = "⊘",
    .frame = {.top_left = "╭",
              .top_right = "╮",
              .bottom_left = "╰",
              .bottom_right = "╯",
              .across = "─",
              .down = "│"},
    .build = "⚙",
    .binary = "◇",
    .merge = "↓",
    .waiting = "‥",
    .queued = "○",
    .spinner = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏",
    .bar_full = "█",
    .bar_empty = "░",
    .bar_eighths = "▏▎▍▌▋▊▉",
    .spark = "▁▂▃▄▅▆▇█",
    .move = "↑↓",
    .enter = "⏎",
    .pages = "←→",
    .trail = "›",
    .cursor = "▌",
    .installed = "●",
    .pick = "★",
    .superscripts = "⁰¹²³⁴⁵⁶⁷⁸⁹",
};

constexpr Glyphs ascii_glyphs{
    .package = "*",
    .selected = "@",
    .system = "@",
    .profile = "@",
    .set = "@",
    .orphan = "-",
    .broken = "!",
    .soname = "~",
    .search = "?",
    .good = "+",
    .choice = "|",
    .branch = "`-",
    .absent = ".",
    .tee = "|-",
    .rail = "| ",
    .folded = "+",
    .unfolded = "-",
    .cycle = "^",
    .instead = ">",
    .upgrade = "U",
    .downgrade = "D",
    .rebuild = "R",
    .added = "N",
    .held = "H",
    .frame = {.top_left = "+",
              .top_right = "+",
              .bottom_left = "+",
              .bottom_right = "+",
              .across = "-",
              .down = "|"},
    .build = "b",
    .binary = "p",
    .merge = "m",
    .waiting = "w",
    .queued = "o",
    .spinner = "|/-\\",
    .bar_full = "#",
    .bar_empty = "-",
    .bar_eighths = "",
    .spark = "_.-~=+*#",
    .move = "j/k",
    .enter = "enter",
    .pages = "h/l",
    .trail = ">",
    .cursor = ">",
    .installed = "+",
    .pick = "*",
    .superscripts = "",
};

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

// The field query.hpp's possible_lines append.
constexpr std::string_view possible_prefix = "use=";

// "a -minimal" as "+a -minimal": the USE flags to set for a possible dependency to appear.
std::string human_toggles(std::string_view toggles) {
    std::string out;
    while (!toggles.empty()) {
        const auto space = toggles.find(' ');
        const auto flag = toggles.substr(0, space);
        out += out.empty() ? "" : " ";
        out += flag.starts_with('-') ? "" : "+";
        out += flag;
        if (space == std::string_view::npos) {
            break;
        }
        toggles.remove_prefix(space + 1);
    }
    return out;
}

std::string spaces(std::size_t used, std::size_t width) {
    return std::string(width > used ? width - used : 0, ' ');
}

std::string count(std::size_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

constexpr const auto& kinds = kind_shorthands;

std::size_t kind_index(std::string_view name) {
    const auto found = std::ranges::find(kinds, name, &KindShorthand::name);
    return static_cast<std::size_t>(found - kinds.begin());
}

std::string kind_letter(std::string_view name, const Painter& paint) {
    const auto index = kind_index(name);
    if (index == kinds.size()) {
        return std::string{name};
    }
    const auto& kind = kinds.at(index);
    return paint(kind.letter, kind.tone);
}

std::string paint_use(std::string_view flags, const Painter& paint) {
    std::string out = paint("[", Tone::note);
    for (std::size_t start = 0; start <= flags.size();) {
        const auto comma = std::min(flags.find(',', start), flags.size());
        out += paint(flags.substr(start, comma - start), Tone::use);
        if (comma < flags.size()) {
            out += paint(",", Tone::note);
        }
        start = comma + 1;
    }
    return out + paint("]", Tone::note);
}

std::string paint_atom(std::string_view text, const Painter& paint) {
    std::string use;
    if (text.ends_with(']')) {
        if (const auto open = text.rfind('['); open != std::string_view::npos) {
            use = paint_use(text.substr(open + 1, text.size() - open - 2), paint);
            text = text.substr(0, open);
        }
    }
    std::string repo;
    if (const auto colons = text.find("::"); colons != std::string_view::npos) {
        repo = paint(text.substr(colons), Tone::repo);
        text = text.substr(0, colons);
    }
    std::string slot;
    if (const auto colon = text.find(':'); colon != std::string_view::npos) {
        slot = paint(text.substr(colon), Tone::slot);
        text = text.substr(0, colon);
    }
    const auto body_start = std::min(text.find_first_not_of("!<>=~"), text.size());
    auto body = text.substr(body_start);
    std::string glob;
    if (body.ends_with('*')) {
        glob = paint("*", Tone::op);
        body.remove_suffix(1);
    }
    return paint(text.substr(0, body_start), Tone::op) + paint_cpv(body, paint) + glob + slot +
           repo + use;
}

} // namespace

std::string Painter::operator()(std::string_view text, Tone tone) const {
    if (depth_ == ColorDepth::none || text.empty()) {
        return std::string{text};
    }
    const auto style = mocha(tone);
    std::string codes = style.bold ? "1;" : "";
    codes += style.italic ? "3;" : "";
    codes += depth_ == ColorDepth::truecolor
                 ? std::format("38;2;{};{};{}", style.red, style.green, style.blue)
                 : std::format("38;5;{}", style.xterm);
    return std::format("\x1b[{}m{}\x1b[0m", codes, text);
}

std::string_view set_glyph(std::string_view set, const Glyphs& glyph) {
    if (set == "@selected") {
        return glyph.selected;
    }
    if (set == "@system") {
        return glyph.system;
    }
    if (set == "@profile") {
        return glyph.profile;
    }
    return glyph.set;
}

const Glyphs& glyphs(GlyphSet set) {
    switch (set) {
    case GlyphSet::nerd:
        return nerd_glyphs;
    case GlyphSet::unicode:
        return unicode_glyphs;
    case GlyphSet::ascii:
        return ascii_glyphs;
    }
    return ascii_glyphs;
}

ToneStyle tone_style(Tone tone) {
    return mocha(tone);
}

CpvParts split_cpv(std::string_view cpv) {
    CpvParts parts;
    const auto slash = cpv.find('/');
    auto rest = cpv;
    if (slash != std::string_view::npos) {
        parts.category = cpv.substr(0, slash);
        rest = cpv.substr(slash + 1);
    }
    for (auto dash = rest.find('-'); dash != std::string_view::npos;
         dash = rest.find('-', dash + 1)) {
        if (parse_version(rest.substr(dash + 1))) {
            parts.name = rest.substr(0, dash);
            parts.version = rest.substr(dash + 1);
            return parts;
        }
    }
    parts.name = rest;
    return parts;
}

std::string paint_cpv(std::string_view cpv, const Painter& paint) {
    const auto parts = split_cpv(cpv);
    std::string out;
    if (!parts.category.empty()) {
        out = paint(parts.category, Tone::category) + paint("/", Tone::note);
    }
    out += paint(parts.name, Tone::name);
    if (!parts.version.empty()) {
        out += paint("-", Tone::note) + paint(parts.version, Tone::version);
    }
    return out;
}

std::string paint_dependency(std::string_view text, const Painter& paint) {
    std::string out;
    while (!text.empty()) {
        const auto space = std::min(text.find(' '), text.size());
        const auto word = text.substr(0, space);
        if (word == "||") {
            out += paint(word, Tone::choice);
        } else if (word == "(" || word == ")") {
            out += paint(word, Tone::note);
        } else {
            out += paint_atom(word, paint);
        }
        if (space < text.size()) {
            out += ' ';
            text.remove_prefix(space + 1);
        } else {
            text = {};
        }
    }
    return out;
}

void human_legend(std::ostream& out, const Theme& theme, bool possible) {
    const auto& paint = theme.paint;
    out << '\n';
    for (const auto& kind : kinds) {
        out << paint(kind.letter, kind.tone) << ' ' << paint(kind.meaning, Tone::note) << "  ";
    }
    out << paint(theme.glyph().choice, Tone::choice) << ' '
        << paint("one alternative of a || group", Tone::note) << '\n';
    if (possible) {
        out << paint("+flag -flag", Tone::note) << ' '
            << paint("USE the ebuild would need to add a possible dependency", Tone::note) << '\n';
    }
}

void human_edges(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> subjects, bool reverse, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    const std::size_t self = reverse ? 3 : 0;
    const std::size_t other = reverse ? 0 : 3;
    bool first = true;
    bool possible = false;
    for (const auto& subject : subjects) {
        // One line per package, atom and toggles, with every kind it appears under.
        struct Line {
            std::string_view package;
            std::string_view atom;
            bool choice = false;
            // Empty unless the dependency is possible.
            std::string_view toggles;
            std::array<bool, kinds.size()> in{};
        };
        std::vector<Line> lines;
        for (const auto& row : rows) {
            if (row.at(self) != subject) {
                continue;
            }
            bool choice = false;
            std::string_view toggles;
            for (const auto field : std::span{row}.subspan(4)) {
                if (field == "any-of") {
                    choice = true;
                } else if (field.starts_with(possible_prefix)) {
                    toggles = field.substr(possible_prefix.size());
                    possible = true;
                }
            }
            auto found = std::ranges::find_if(lines, [&](const Line& line) {
                return line.package == row.at(other) && line.atom == row.at(2) &&
                       line.choice == choice && line.toggles == toggles;
            });
            if (found == lines.end()) {
                lines.push_back({.package = row.at(other),
                                 .atom = row.at(2),
                                 .choice = choice,
                                 .toggles = toggles});
                found = lines.end() - 1;
            }
            if (const auto index = kind_index(row.at(1)); index < kinds.size()) {
                found->in.at(index) = true;
            }
        }
        std::ranges::stable_sort(lines, {}, &Line::package);

        out << (first ? "" : "\n") << paint(glyph.package, Tone::heading) << ' '
            << paint_cpv(subject, paint) << "  "
            << paint(count(lines.size(), reverse ? "dependent" : "dependency",
                           reverse ? "dependents" : "dependencies"),
                     Tone::note)
            << '\n';
        first = false;
        std::size_t width = 0;
        for (const auto& line : lines) {
            width = std::max(width, line.package.size());
        }
        for (const auto& line : lines) {
            out << "  ";
            for (std::size_t k = 0; k < kinds.size(); ++k) {
                out << (line.in.at(k) ? paint(kinds.at(k).letter, kinds.at(k).tone)
                                      : paint(glyph.absent, Tone::note));
            }
            out << "  " << paint_cpv(line.package, paint) << spaces(line.package.size(), width)
                << "  " << paint_dependency(line.atom, paint);
            if (line.choice) {
                out << ' ' << paint(glyph.choice, Tone::choice);
            }
            if (!line.toggles.empty()) {
                out << "  " << paint(human_toggles(line.toggles), Tone::note);
            }
            out << '\n';
        }
    }
    if (!rows.empty()) {
        human_legend(out, theme, possible);
    }
}

void human_path(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    if (rows.empty()) {
        return;
    }
    const auto& root = rows.front();
    const auto target = rows.size() > 1 ? rows.back().at(3) : root.at(2);
    out << paint(glyph.package, Tone::heading) << ' ' << paint_cpv(target, paint) << "  "
        << paint("is kept by", Tone::note) << '\n';
    out << paint(set_glyph(root.at(0), glyph), Tone::root) << ' ' << paint(root.at(0), Tone::root)
        << "  " << paint_dependency(root.at(1), paint) << '\n';
    // Each package one level deeper than the one depending on it; the branch and its space take
    // three columns.
    const auto indent = [](std::size_t depth) { return 3 * depth; };
    std::size_t width = indent(1) + root.at(2).size();
    for (std::size_t i = 1; i < rows.size(); ++i) {
        width = std::max(width, indent(i + 1) + rows.at(i).at(3).size());
    }
    const auto line = [&](std::size_t depth, std::string_view package) {
        out << std::string(indent(depth), ' ') << paint(glyph.branch, Tone::note) << ' '
            << paint_cpv(package, paint);
    };
    line(0, root.at(2));
    out << '\n';
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& edge = rows.at(i);
        line(i, edge.at(3));
        out << spaces(indent(i + 1) + edge.at(3).size(), width) << "  "
            << kind_letter(edge.at(1), paint) << "  " << paint_dependency(edge.at(2), paint);
        if (edge.size() > 4) {
            out << ' ' << paint(glyph.choice, Tone::choice);
        }
        out << '\n';
    }
}

void human_orphans(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    if (records.empty()) {
        out << paint(theme.glyph().good, Tone::good) << ' '
            << paint("Nothing to remove.", Tone::good) << '\n';
        return;
    }
    for (const auto& cpv : records) {
        out << paint(theme.glyph().orphan, Tone::bad) << ' ' << paint_cpv(cpv, paint) << '\n';
    }
    out << '\n'
        << paint(std::to_string(records.size()), Tone::count)
        << paint(records.size() == 1 ? " package depclean would remove"
                                     : " packages depclean would remove",
                 Tone::note)
        << '\n';
}

void human_verification(std::ostream& out, std::span<const std::string> records, const Theme& theme,
                        std::string_view verb) {
    const auto& paint = theme.paint;
    out << '\n';
    if (records.empty()) {
        out << paint(theme.glyph().good, Tone::good) << ' '
            << paint(std::format("emerge --pretend {} the same.", verb), Tone::good) << '\n';
        return;
    }
    out << paint(theme.glyph().broken, Tone::bad) << ' '
        << paint(std::format("emerge --pretend {} otherwise:", verb), Tone::bad) << '\n';
    const auto rows = split_all(records);
    std::size_t width = 0;
    for (const auto& row : rows) {
        width = std::max(width, row.front().size());
    }
    for (const auto& row : rows) {
        const auto key = row.front();
        // A blocker's holder or an unsatisfied atom has no repository.
        const auto colons = std::min(key.find("::"), key.size());
        out << "  " << paint_cpv(key.substr(0, colons), paint)
            << paint(key.substr(colons), Tone::repo) << std::string(width - key.size() + 2, ' ');
        const auto what = row.at(1);
        if (what == "egraph") {
            out << paint(std::format("only here ({})", row.at(2)), Tone::note);
        } else if (what == "emerge") {
            out << paint(std::format("only in emerge ({})", row.at(2)), Tone::note);
        } else if (what == "kind") {
            out << paint(std::format("{} here, {} in emerge", row.at(2), row.at(3)), Tone::note);
        } else {
            out << paint(row.at(2), Tone::use) << paint(" here", Tone::note) << '\n'
                << std::string(width + 4, ' ') << paint(row.at(3), Tone::use)
                << paint(" in emerge", Tone::note);
        }
        out << '\n';
    }
}

void human_removal(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    std::size_t width = 0;
    std::size_t removed = 0;
    for (const auto& row : rows) {
        if (row.at(1) == "kept") {
            width = std::max(width, row.front().size());
        }
    }
    for (const auto& row : rows) {
        const auto cpv = row.front();
        if (row.at(1) == "remove") {
            ++removed;
            out << paint(theme.glyph().orphan, Tone::bad) << ' ' << paint_cpv(cpv, paint) << '\n';
            continue;
        }
        std::vector<std::string_view> dependents;
        std::vector<std::string_view> sets;
        for (const auto by : std::span{row}.subspan(2)) {
            (by.starts_with('@') ? sets : dependents).push_back(by);
        }
        out << paint(theme.glyph().held, Tone::note) << ' ' << paint_cpv(cpv, paint)
            << std::string(width - cpv.size() + 2, ' ')
            << paint(holder_note(dependents, sets), Tone::note) << '\n';
    }
    const auto kept = rows.size() - removed;
    out << '\n';
    if (removed == 0) {
        out << paint("Nothing to remove", Tone::note);
    } else {
        out << paint(std::to_string(removed), Tone::count)
            << paint(removed == 1 ? " package to remove" : " packages to remove", Tone::note);
    }
    if (kept > 0) {
        out << paint(", ", Tone::note) << paint(std::to_string(kept), Tone::count)
            << paint(" kept", Tone::note);
    }
    out << '\n';
}

void human_deselect(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    std::size_t atoms = 0;
    for (const auto& row : rows) {
        if (row.at(1) == "deselect") {
            ++atoms;
            const auto atom = row.front();
            out << paint(theme.glyph().orphan, Tone::bad) << ' '
                << (atom.starts_with('@') ? paint(atom, Tone::root) : paint_cpv(atom, paint))
                << '\n';
        }
    }
    out << '\n'
        << paint(std::to_string(atoms), Tone::count)
        << paint(atoms == 1 ? " atom leaves @selected" : " atoms leave @selected", Tone::note);
    if (atoms == rows.size()) {
        out << '\n';
        return;
    }
    out << paint("; then emerge --depclean would remove:", Tone::note) << '\n';
    for (const auto& row : rows) {
        if (row.at(1) == "orphan") {
            out << paint(theme.glyph().orphan, Tone::bad) << ' ' << paint_cpv(row.front(), paint)
                << '\n';
        }
    }
}

void human_selection(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    if (records.empty()) {
        return;
    }
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    out << '\n';
    for (const auto* change : {"selected", "deselected"}) {
        for (const auto& row : rows) {
            if (row.at(1) != change) {
                continue;
            }
            const bool joined = row.at(1) == "selected";
            out << paint(joined ? theme.glyph().good : theme.glyph().orphan,
                         joined ? Tone::good : Tone::bad)
                << ' ' << paint_cpv(row.front(), paint)
                << paint(joined ? " joined @selected" : " left @selected", Tone::note) << '\n';
        }
    }
}

void human_elog(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    std::string_view package;
    // The class and phase of the message the lines belong to, shown once.
    std::pair<std::string_view, std::string_view> message;
    for (const auto& row : split_all(records)) {
        if (row.front() != package) {
            package = row.front();
            message = {};
            out << '\n'
                << paint("Messages for ", Tone::heading) << paint_cpv(package, paint) << ":\n";
        }
        if (const std::pair here{row.at(2), row.at(3)}; here != message) {
            message = here;
            const bool loud = here.first == "ERROR" || here.first == "WARN";
            out << "  "
                << paint(std::format("{} ({})", here.second, here.first),
                         loud ? Tone::bad : Tone::note)
                << '\n';
        }
        out << (row.at(4).empty() ? "" : "    ") << row.at(4) << '\n';
    }
}

void human_notices(std::ostream& out, std::span<const std::string> records, const Theme& theme,
                   Seconds now) {
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    bool printed = false;
    const auto heading = [&](std::string_view title, std::string_view note) {
        out << (printed ? "\n" : "") << paint(title, Tone::heading) << paint(note, Tone::note)
            << ":\n";
        printed = true;
    };
    std::string_view advisory;
    for (const auto& row : rows) {
        if (row.at(1) != "glsa") {
            continue;
        }
        if (!printed) {
            heading("Security advisories", " (egraph install -1 the fixed versions)");
        }
        if (row.front() != advisory) {
            advisory = row.front();
            out << "  " << paint(advisory, Tone::bad) << "  " << row.at(2) << '\n';
        }
        out << "    " << paint_cpv(row.at(3), paint);
        bool first = true;
        for (const auto part : std::views::split(row.at(4), ' ')) {
            if (const std::string_view atom{part}; !atom.empty()) {
                out << paint(first ? ", fixed in " : " or ", Tone::note) << atom;
                first = false;
            }
        }
        out << '\n';
    }
    // Each file once, with how many updates wait for it; rows come grouped by file.
    std::vector<std::pair<std::string_view, std::size_t>> files;
    for (const auto& row : rows) {
        if (row.at(1) != "config") {
            continue;
        }
        if (files.empty() || files.back().first != row.front()) {
            files.emplace_back(row.front(), 0);
        }
        ++files.back().second;
    }
    if (!files.empty()) {
        heading("Configuration updates", " (dispatch-conf)");
        for (const auto& [file, count] : files) {
            out << "  " << file;
            if (count > 1) {
                out << "  " << paint(std::to_string(count), Tone::count)
                    << paint(" updates", Tone::note);
            }
            out << '\n';
        }
    }
    for (const auto& row : rows) {
        if (row.at(1) != "plan") {
            continue;
        }
        if (row.front() == "title") {
            heading(row.at(2), " (egraph updates)");
            continue;
        }
        const std::string_view line = row.at(2);
        const auto tone = line.starts_with("+ ")   ? Tone::good
                          : line.starts_with("- ") ? Tone::bad
                                                   : Tone::note;
        out << "  " << paint(line, tone) << '\n';
    }
    bool first = true;
    for (const auto& row : rows) {
        if (row.at(1) != "check") {
            continue;
        }
        if (first) {
            heading("Configuration check", " (egraph config check)");
            first = false;
        }
        std::string counts;
        for (const auto& [field, one] :
             {std::pair{row.at(2), "error"}, {row.at(3), "warning"}, {row.at(4), "note"}}) {
            if (field != "0") {
                counts += std::format("{}{} {}{}", counts.empty() ? "" : ", ", field, one,
                                      field == "1" ? "" : "s");
            }
        }
        out << "  " << row.front() << "  " << paint(counts, Tone::count) << '\n';
    }
    first = true;
    for (const auto& row : rows) {
        if (row.at(1) != "news") {
            continue;
        }
        if (first) {
            heading("Unread news", " (eselect news read)");
            first = false;
        }
        out << "  " << paint(row.front(), Tone::note);
        if (const auto title = row.at(3); !title.empty()) {
            out << "  " << title;
        }
        out << '\n';
    }
    first = true;
    for (const auto& row : rows) {
        if (row.at(1) != "preserved") {
            continue;
        }
        if (first) {
            heading("Preserved libraries", " (egraph install -1 @preserved-rebuild)");
            first = false;
        }
        out << "  " << row.front() << "  " << paint("from ", Tone::note)
            << paint_cpv(row.at(2), paint);
        bool consumer_first = true;
        for (const auto part : std::views::split(row.at(3), ' ')) {
            if (const std::string_view consumer{part}; !consumer.empty()) {
                out << paint(consumer_first ? ", used by " : ", ", Tone::note)
                    << paint_cpv(consumer, paint);
                consumer_first = false;
            }
        }
        out << '\n';
    }
    first = true;
    for (const auto& row : rows) {
        if (row.at(1) != "stale") {
            continue;
        }
        if (first) {
            heading("Stale repositories", " (egraph sync)");
            first = false;
        }
        const auto digits = row.at(2);
        std::int64_t seconds = 0;
        std::from_chars(digits.begin(), digits.end(), seconds);
        out << "  " << row.front() << "  "
            << paint(
                   std::format("synced {}", age_text(Seconds{std::chrono::seconds{seconds}}, now)),
                   Tone::note)
            << '\n';
    }
    first = true;
    for (const auto& row : rows) {
        if (row.at(1) != "masked") {
            continue;
        }
        if (first) {
            heading("Masked installed packages", "");
            first = false;
        }
        out << "  " << paint_cpv(row.front(), paint) << "  " << paint(row.at(2), Tone::note)
            << '\n';
    }
    first = true;
    for (const auto& row : rows) {
        if (row.at(1) != "missing") {
            continue;
        }
        if (first) {
            heading("Missing libraries", " (rebuild what needs them)");
            first = false;
        }
        out << "  " << paint_cpv(row.front(), paint) << paint("  needs ", Tone::note) << row.at(3)
            << paint(std::format(" ({})", row.at(2)), Tone::note) << '\n';
    }
}

std::string holder_note(std::span<const std::string_view> dependents,
                        std::span<const std::string_view> sets) {
    std::vector<std::string_view> others;
    bool selected = false;
    for (const auto set : sets) {
        if (set == "@selected") {
            selected = true;
        } else if (!std::ranges::contains(others, set)) {
            others.push_back(set);
        }
    }
    std::string note;
    if (!dependents.empty()) {
        constexpr std::size_t shown = 3;
        note = "needed by ";
        for (const auto dependent : dependents.first(std::min(shown, dependents.size()))) {
            note += std::format("{}{}", note.ends_with(' ') ? "" : ", ", dependent);
        }
        if (dependents.size() > shown) {
            note += std::format(" and {} more", dependents.size() - shown);
        }
    }
    if (!others.empty()) {
        note += note.empty() ? "kept by " : "; kept by ";
        for (std::size_t i = 0; i < others.size(); ++i) {
            note += std::format("{}{}", i == 0 ? "" : ", ", others.at(i));
        }
    }
    if (note.empty()) {
        note = selected ? "nothing depends on it; only @selected keeps it"
                        : "nothing depends on it or keeps it";
    }
    return note;
}

std::vector<RemedyLine> remedy_lines(std::span<const std::string_view> holders,
                                     std::span<const std::string_view> deselect,
                                     std::string_view target,
                                     const std::optional<std::vector<std::string_view>>& frees,
                                     bool nodeps) {
    const auto them = holders.size() == 1 ? "it" : "them";
    std::vector<RemedyLine> lines;
    if (frees) {
        std::string label = std::format("to remove {}:", them);
        const auto add = [&](std::string text, Tone tone) {
            lines.push_back(
                {.label = std::exchange(label, {}), .text = std::move(text), .tone = tone});
        };
        if (!deselect.empty()) {
            std::string atoms;
            for (const auto atom : deselect) {
                atoms += std::format(" {}", atom);
            }
            add("emerge --deselect" + atoms, Tone::use);
        }
        std::string unmerge;
        for (const auto holder : holders) {
            unmerge += std::format(" ={}", holder);
        }
        add("emerge -C" + unmerge, Tone::use);
        add(std::format("emerge -1 ={}", target), Tone::use);
        if (!frees->empty()) {
            std::string freed;
            for (const auto cpv : *frees) {
                freed += std::format("{}{}", freed.empty() ? "" : ", ", cpv);
            }
            add("which also frees " + freed, Tone::good);
        }
    }
    if (nodeps) {
        lines.push_back({.label = std::format("to keep {}:", them),
                         .text = std::format("emerge -1 --nodeps ={}", target),
                         .tone = Tone::use});
        lines.push_back(
            {.label = {}, .text = "which a later emerge -uD undoes", .tone = Tone::bad});
    }
    return lines;
}

namespace {

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> found;
    if (!text.empty()) {
        for (const auto part : std::views::split(text, ' ')) {
            found.emplace_back(part);
        }
    }
    return found;
}

// A holder's remedy line fields (dependents, then "@set atom" per root) as a sentence.
std::string holder_fields_note(const Fields& fields) {
    std::vector<std::string_view> sets;
    for (std::size_t i = 1; i < fields.size(); ++i) {
        sets.push_back(fields.at(i).substr(0, fields.at(i).find(' ')));
    }
    return holder_note(fields.empty() ? std::vector<std::string_view>{} : words(fields.front()),
                       sets);
}

void put_remedies(std::ostream& out, std::string_view indent, const Fields& row,
                  const std::map<std::string_view, Fields, std::less<>>& holders,
                  std::optional<std::string_view> frees, bool nodeps, const Painter& paint) {
    std::vector<std::string_view> cpvs;
    std::vector<std::string_view> deselect;
    for (const auto& [holder, fields] : holders) {
        cpvs.push_back(holder);
        for (std::size_t i = 1; i < fields.size(); ++i) {
            const auto space = fields.at(i).find(' ');
            if (fields.at(i).substr(0, space) == "@selected") {
                deselect.push_back(fields.at(i).substr(space + 1));
            }
        }
    }
    const auto lines = remedy_lines(cpvs, deselect, row.at(2),
                                    frees ? std::optional{words(*frees)} : std::nullopt, nodeps);
    std::size_t width = 0;
    for (const auto& line : lines) {
        width = std::max(width, line.label.size() + 1);
    }
    for (const auto& line : lines) {
        out << indent << "    " << paint(line.label, Tone::heading)
            << spaces(line.label.size(), width) << paint(line.text, line.tone) << '\n';
    }
}

// The places of a table row's waits that hold it after the merge there (ordering, in plan.hpp):
// those before its own place, without their kinds.
std::string earlier_places(std::string_view place, std::string_view waits) {
    // Places have no leading zeros, so the shorter is the smaller.
    const auto before = [](std::string_view a, std::string_view b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    };
    std::string places;
    for (const auto wait : words(waits)) {
        const auto digits = std::min(wait.find_first_not_of("0123456789"), wait.size());
        const auto letters = wait.substr(digits);
        if (before(wait.substr(0, digits), place) &&
            letters.find_first_of("birp") != letters.npos) {
            places += std::format("{}{}", places.empty() ? "" : " ", wait.substr(0, digits));
        }
    }
    return places;
}

// A new-slot row's installed packages ("cpv:slot ..."), as "beside version:slot ...".
void put_beside(std::ostream& out, std::string_view beside, const Painter& paint) {
    out << "  " << paint("beside", Tone::note);
    for (const auto each : words(beside)) {
        const auto colon = std::min(each.find(':'), each.size());
        out << ' ' << paint(split_cpv(each.substr(0, colon)).version, Tone::version)
            << paint(each.substr(colon), Tone::note);
    }
}

} // namespace

void human_updates(std::ostream& out, std::span<const std::string> records, const Theme& theme,
                   bool table, std::size_t unlisted) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    auto rows = split_all(records);
    // A table row's place and the earlier places it waits for, taken off ahead of its fields.
    std::vector<std::pair<std::string_view, std::string>> places(rows.size());
    std::size_t place_width = 0;
    std::size_t waits_width = 0;
    if (table) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows.at(i);
            // An uninstall waits for merges in any place, and lists them after its own fields.
            if (row.size() > 3 && row.at(3) == "uninstall") {
                places.at(i) = {row.at(0), std::string(row.at(1))};
                row.erase(row.begin(), row.begin() + 2);
                continue;
            }
            places.at(i) = {row.at(0), earlier_places(row.at(0), row.at(1))};
            row.erase(row.begin(), row.begin() + 2);
            place_width = std::max(place_width, places.at(i).first.size());
            if (!places.at(i).second.empty()) {
                waits_width =
                    std::max(waits_width, glyph.waiting.size() + 1 + places.at(i).second.size());
            }
        }
    }
    // Remedies, taken off by the held cpv they follow.
    struct Remedies {
        // Holder cpv, then its dependents and each "@set atom" selecting it.
        std::map<std::string_view, Fields, std::less<>> holders;
        std::optional<std::string_view> frees;
        bool nodeps = false;
    };
    std::map<std::string_view, Remedies, std::less<>> remedies;
    for (std::size_t i = rows.size(); i-- > 0;) {
        const auto& row = rows.at(i);
        const auto kind = row.size() > 1 ? row.at(1) : std::string_view{};
        if (kind != "holder" && kind != "remove" && kind != "nodeps") {
            continue;
        }
        auto& found = remedies[row.at(0)];
        if (kind == "holder") {
            found.holders.emplace(row.at(2), Fields(row.begin() + 3, row.end()));
        } else if (kind == "remove") {
            found.frees = row.at(2);
        } else {
            found.nodeps = true;
        }
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
        places.erase(places.begin() + static_cast<std::ptrdiff_t>(i));
    }
    // What lines tried change, taken off: the marks of the merges by first field, and the merges
    // they drop, as merge rows.
    std::map<std::string_view, std::string_view, std::less<>> tried;
    std::vector<Fields> dropped;
    bool trying = false;
    // Merges added, changed, and of those rebuilds for USE.
    std::size_t more = 0;
    std::size_t changed = 0;
    std::size_t for_use = 0;
    for (std::size_t i = rows.size(); i-- > 0;) {
        const auto& row = rows.at(i);
        if (row.size() < 3 || row.at(1) != "tried") {
            continue;
        }
        trying = true;
        if (row.at(2) == "dropped" && row.size() > 6) {
            dropped.push_back({row.at(0), row.at(3), row.at(4), row.at(5)});
        } else if (row.at(2) != "none" && row.size() > 6) {
            tried.emplace(row.at(0), row.at(2));
            ++(row.at(2) == "added" ? more : changed);
            if (row.at(3) == "rebuild" && !row.at(6).empty()) {
                ++for_use;
            }
        }
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
        places.erase(places.begin() + static_cast<std::ptrdiff_t>(i));
    }
    std::ranges::reverse(dropped);
    // The installed packages lines tried build with other env files, taken off.
    std::vector<Fields> envs;
    for (std::size_t i = rows.size(); i-- > 0;) {
        if (rows.at(i).size() < 4 || rows.at(i).at(1) != "env") {
            continue;
        }
        envs.push_back(std::move(rows.at(i)));
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
        places.erase(places.begin() + static_cast<std::ptrdiff_t>(i));
    }
    std::ranges::reverse(envs);
    // Uninstalls, blocks, unsatisfied dependencies and unmet REQUIRED_USE, taken off: they share
    // no columns with the merges.
    std::vector<Fields> uninstalls;
    std::vector<std::string> uninstall_waits;
    std::vector<Fields> blocks;
    std::vector<Fields> unsatisfied;
    std::vector<Fields> unmet;
    std::vector<Fields> use_changes;
    std::vector<Fields> masked;
    for (std::size_t i = rows.size(); i-- > 0;) {
        const auto kind = rows.at(i).size() > 1 ? rows.at(i).at(1) : std::string_view{};
        if (kind != "uninstall" && kind != "blocks" && kind != "unsatisfied" &&
            kind != "required-use" && kind != "use-change" && kind != "masked") {
            continue;
        }
        auto& into = kind == "uninstall"      ? uninstalls
                     : kind == "blocks"       ? blocks
                     : kind == "unsatisfied"  ? unsatisfied
                     : kind == "required-use" ? unmet
                     : kind == "masked"       ? masked
                                              : use_changes;
        into.push_back(std::move(rows.at(i)));
        if (kind == "uninstall") {
            uninstall_waits.push_back(std::move(places.at(i).second));
        }
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
        places.erase(places.begin() + static_cast<std::ptrdiff_t>(i));
    }
    std::ranges::reverse(uninstalls);
    std::ranges::reverse(masked);
    std::ranges::reverse(uninstall_waits);
    std::ranges::reverse(blocks);
    std::ranges::reverse(unsatisfied);
    std::ranges::reverse(unmet);
    std::ranges::reverse(use_changes);
    const bool refusals = !unsatisfied.empty() || !unmet.empty() || !use_changes.empty();
    const auto is_held = [](const auto& row) { return row.at(1) == "held"; };
    const auto is_new = [](const auto& row) {
        return row.at(1) == "new" || row.at(1) == "new-slot";
    };
    // Every row shares the columns, so held ones line up under the updates.
    std::size_t cp_width = 0;
    std::size_t version_width = 0;
    // " > version" when any row moves to another version, so every repo lines up.
    std::size_t move_width = 0;
    const auto widen = [&](const Fields& row) {
        const auto parts = split_cpv(row.at(0));
        cp_width = std::max(cp_width, parts.category.size() + 1 + parts.name.size());
        version_width = std::max(version_width, parts.version.size());
        if (row.at(0) != row.at(2)) {
            move_width = std::max(move_width, 3 + split_cpv(row.at(2)).version.size());
        }
    };
    std::ranges::for_each(rows, widen);
    std::ranges::for_each(dropped, widen);
    bool flags = false;
    bool fixed = false;
    // The package, its version and where it goes, its repository, and any flags.
    const auto put_row = [&](std::size_t index, std::string_view mark, Tone tone) {
        const auto& row = rows.at(index);
        if (table) {
            const auto place = places.at(index).first;
            out << spaces(place.size(), place_width) << paint(place, Tone::count) << ' ';
        }
        const auto old = split_cpv(row.at(0));
        const auto target = split_cpv(row.at(2));
        const auto cp = row.at(0).substr(0, old.category.size() + 1 + old.name.size());
        if (trying) {
            const auto change = is_held(row) ? tried.end() : tried.find(row.at(0));
            out << (change == tried.end()       ? std::string{"  "}
                    : change->second == "added" ? paint("+", Tone::good) + ' '
                                                : paint("~", Tone::use) + ' ');
        }
        out << paint(mark, tone) << ' ' << paint_cpv(cp, paint) << spaces(cp.size(), cp_width)
            << "  " << paint(old.version, Tone::version)
            << spaces(old.version.size(), version_width);
        if (row.at(0) != row.at(2)) {
            const bool down = row.at(1) == "downgrade";
            out << ' ' << paint(glyph.instead, Tone::note) << ' '
                << paint(target.version, down ? Tone::bad : Tone::good)
                << spaces(3 + target.version.size(), move_width);
        } else {
            out << spaces(0, move_width);
        }
        out << "  " << paint("::" + std::string{row.at(3)}, Tone::repo);
        // The waits column's padding, written only when something follows it.
        std::size_t pad = 0;
        if (waits_width != 0) {
            const auto& waits = places.at(index).second;
            if (waits.empty()) {
                pad = 2 + waits_width;
            } else {
                out << "  " << paint(glyph.waiting, Tone::note) << ' ' << paint(waits, Tone::count);
                pad = waits_width - (glyph.waiting.size() + 1 + waits.size());
            }
        }
        // A package, then its atom: what pulls a new one in, or what a rebuild is for.
        const auto put_why = [&](std::string_view by) {
            const auto cut = std::min(by.find(' '), by.size());
            out << std::string(pad, ' ') << "  " << paint(by.substr(0, cut), Tone::version) << ' '
                << paint(by.substr(std::min(cut + 1, by.size())), Tone::note);
        };
        if (is_new(row)) {
            if (row.size() > 5 && !row.at(5).empty()) {
                put_why(row.at(5));
            } else if (row.size() > 6) {
                out << std::string(pad, ' ');
            }
            if (row.size() > 6) {
                put_beside(out, row.at(6), paint);
            }
        } else if (row.at(1) == "rebuild" && row.size() > 5) {
            put_why(row.at(5));
        } else if (row.size() > 4 && !row.at(4).empty()) {
            flags = true;
            out << std::string(pad, ' ') << ' ';
            for (const auto flag : std::views::split(row.at(4), ' ')) {
                const std::string_view text{flag};
                out << ' ' << paint(text, text.contains('*') ? Tone::use : Tone::note);
            }
        }
        out << '\n';
        // A new package's USE on a line of its own: VARIABLE="flag -flag (fixed)" groups.
        if (is_new(row) && row.size() > 4 && !row.at(4).empty()) {
            out << std::string(place_width == 0 ? 3 : place_width + 4, ' ');
            for (const auto token : std::views::split(row.at(4), ' ')) {
                std::string_view flag{token};
                if (const auto open = flag.find("=\""); open != std::string_view::npos) {
                    out << ' ' << paint(flag.substr(0, open + 2), Tone::note);
                    flag.remove_prefix(open + 2);
                } else {
                    out << ' ';
                }
                const bool close = flag.ends_with('"');
                if (close) {
                    flag.remove_suffix(1);
                }
                fixed = fixed || flag.starts_with('(');
                const bool off = flag.starts_with('-') || flag.starts_with("(-");
                out << paint(flag, off ? Tone::note : Tone::use);
                if (close) {
                    out << paint("\"", Tone::note);
                }
            }
            out << '\n';
        }
    };
    // up, down, rebuild, new, new slot, held
    std::array<std::size_t, 6> counts{};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows.at(index);
        if (is_held(row)) {
            ++counts.at(5);
            continue;
        }
        if (is_new(row)) {
            ++counts.at(row.at(1) == "new" ? 3 : 4);
            // In merge order, among the rest.
            if (table) {
                put_row(index, glyph.added, Tone::good);
            }
            continue;
        }
        const bool up = row.at(1) == "upgrade";
        const bool down = row.at(1) == "downgrade";
        ++counts.at(up ? 0 : down ? 1 : 2);
        put_row(index,
                up     ? glyph.upgrade
                : down ? glyph.downgrade
                       : glyph.rebuild,
                up     ? Tone::good
                : down ? Tone::bad
                       : Tone::use);
    }
    if (counts.at(0) + counts.at(1) + counts.at(2) + counts.at(3) + counts.at(4) == 0) {
        if (!refusals) {
            out << paint(glyph.good, Tone::good) << ' ' << paint("Nothing to update.", Tone::good)
                << '\n';
        }
        // emerge warns of masked installed packages with nothing to merge too.
        if (counts.at(5) + unlisted == 0 && !refusals && masked.empty() && !trying) {
            return;
        }
    }
    if (counts.at(3) + counts.at(4) != 0 && !table) {
        out << '\n' << paint("New", Tone::heading) << '\n';
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (is_new(rows.at(index))) {
                put_row(index, glyph.added, Tone::good);
            }
        }
    }
    // The merges lines tried drop, dimmed.
    for (const auto& row : dropped) {
        if (table) {
            out << std::string(place_width + 1, ' ');
        }
        const auto old = split_cpv(row.at(0));
        const auto cp = row.at(0).substr(0, old.category.size() + 1 + old.name.size());
        const auto kind = row.at(1);
        const auto mark = kind == "upgrade"     ? glyph.upgrade
                          : kind == "downgrade" ? glyph.downgrade
                          : kind == "rebuild"   ? glyph.rebuild
                                                : glyph.added;
        std::string text = std::format("{} {}{}  {}{}", mark, cp, spaces(cp.size(), cp_width),
                                       old.version, spaces(old.version.size(), version_width));
        if (row.at(0) != row.at(2)) {
            const auto target = split_cpv(row.at(2)).version;
            text += std::format(" {} {}{}", glyph.instead, target,
                                spaces(3 + target.size(), move_width));
        } else {
            text += spaces(0, move_width);
        }
        out << paint("-", Tone::bad) << ' '
            << paint(std::format("{}  ::{}  (dropped)", text, row.at(3)), Tone::note) << '\n';
    }
    if (!envs.empty()) {
        out << '\n'
            << paint("Built differently from now on", Tone::heading) << ' '
            << paint("(package.env)", Tone::note) << '\n';
        std::size_t width = 0;
        for (const auto& row : envs) {
            width = std::max(width, row.at(0).size());
        }
        bool unmerged = false;
        for (const auto& row : envs) {
            out << "  " << paint_cpv(row.at(0), paint) << spaces(row.at(0).size(), width) << "  "
                << paint(row.at(3).empty() ? "no env file" : row.at(3), Tone::use) << ' '
                << paint(std::format("(was {})", row.at(2).empty() ? "none" : row.at(2)),
                         Tone::note)
                << '\n';
            unmerged = unmerged || std::ranges::none_of(rows, [&](const Fields& merge) {
                           return merge.at(0) == row.at(0) && merge.at(1) != "held";
                       });
        }
        if (unmerged) {
            out << paint("rebuild them: --rebuild-env", Tone::note) << '\n';
        }
    }
    if (counts.at(5) != 0) {
        out << '\n' << paint("Held back", Tone::heading) << '\n';
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows.at(index);
            if (!is_held(row)) {
                continue;
            }
            put_row(index, glyph.held, Tone::bad);
            // One line per dependent holding it: its cpv, then its atoms that do.
            std::size_t holder_width = 0;
            for (std::size_t i = 5; i < row.size(); ++i) {
                holder_width =
                    std::max(holder_width, row.at(i).substr(0, row.at(i).find(' ')).size());
            }
            const auto indent = std::string(place_width == 0 ? 0 : place_width + 1, ' ');
            const auto found = remedies.find(row.at(0));
            for (std::size_t i = 5; i < row.size(); ++i) {
                const auto field = row.at(i);
                const auto holder = field.substr(0, field.find(' '));
                out << indent << "    " << paint(holder, Tone::version)
                    << spaces(holder.size(), holder_width);
                if (holder.size() < field.size()) {
                    for (const auto atom :
                         std::views::split(field.substr(holder.size() + 1), ' ')) {
                        out << "  " << paint(std::string_view{atom}, Tone::note);
                    }
                }
                out << '\n';
                if (found != remedies.end()) {
                    if (const auto info = found->second.holders.find(holder);
                        info != found->second.holders.end()) {
                        out << indent << "      "
                            << paint(holder_fields_note(info->second), Tone::note) << '\n';
                    }
                }
            }
            if (found != remedies.end()) {
                put_remedies(out, indent, row, found->second.holders, found->second.frees,
                             found->second.nodeps, paint);
            }
        }
    }
    // A package, then a blocker's atom.
    const auto put_blocker = [&](std::string_view cpv, std::string_view atom) {
        out << paint_cpv(cpv, paint) << ' ' << paint(atom, Tone::bad);
    };
    if (!uninstalls.empty()) {
        std::size_t width = 0;
        for (const auto& row : uninstalls) {
            width = std::max(width, row.at(0).size());
        }
        out << '\n' << paint("Uninstalled", Tone::heading) << '\n';
        for (std::size_t i = 0; i < uninstalls.size(); ++i) {
            const auto& row = uninstalls.at(i);
            out << paint(glyph.orphan, Tone::bad) << ' ' << paint_cpv(row.at(0), paint)
                << spaces(row.at(0).size(), width) << "  ";
            // A replaced slot, blocked by a merge, or blocking one itself.
            if (row.at(2).empty()) {
                out << paint("replaced by", Tone::note) << ' ' << paint_cpv(row.at(4), paint);
            } else if (row.at(2) == row.at(0)) {
                out << paint("blocks", Tone::note) << ' ';
                put_blocker(row.at(4), row.at(3));
            } else {
                out << paint("blocked by", Tone::note) << ' ';
                put_blocker(row.at(2), row.at(3));
            }
            if (const auto& waits = uninstall_waits.at(i); !waits.empty()) {
                out << "  " << paint(glyph.waiting, Tone::note) << ' ' << paint(waits, Tone::count);
            }
            out << '\n';
        }
    }
    if (!blocks.empty()) {
        out << '\n' << paint("Blocked", Tone::heading) << '\n';
        for (const auto& row : blocks) {
            out << paint(glyph.broken, Tone::bad) << ' ';
            put_blocker(row.at(0), row.at(2));
            out << "  " << paint("blocks", Tone::note) << ' ' << paint_cpv(row.at(3), paint)
                << '\n';
        }
        out << paint("emerge refuses a plan with blockers it cannot resolve", Tone::bad) << '\n';
    }
    if (!unsatisfied.empty()) {
        std::size_t width = 0;
        for (const auto& row : unsatisfied) {
            width = std::max(width, row.at(0).size());
        }
        out << '\n' << paint("Unsatisfied", Tone::heading) << '\n';
        for (const auto& row : unsatisfied) {
            out << paint(glyph.broken, Tone::bad) << ' ' << paint_cpv(row.at(0), paint)
                << spaces(row.at(0).size(), width) << "  " << paint("needs", Tone::note) << ' '
                << paint(row.at(2), Tone::bad) << '\n';
        }
        out << paint("no visible version matches: emerge refuses the plan", Tone::bad) << '\n';
    }
    if (!unmet.empty()) {
        out << '\n' << paint("Unmet REQUIRED_USE", Tone::heading) << '\n';
        for (const auto& row : unmet) {
            out << paint(glyph.broken, Tone::bad) << ' ' << paint_cpv(row.at(0), paint)
                << paint(std::format("::{}", row.at(2)), Tone::repo);
            if (!row.at(3).empty()) {
                out << "  " << paint(row.at(3), Tone::use);
            }
            out << "\n    " << paint(human_readable_required_use(row.at(4)), Tone::bad) << '\n';
            if (row.size() > 5 && !row.at(5).empty()) {
                out << "    " << paint("of", Tone::note) << ' '
                    << paint(human_readable_required_use(row.at(5)), Tone::note) << '\n';
            }
        }
        out << paint("its USE leaves REQUIRED_USE unsatisfied: emerge refuses the plan", Tone::bad)
            << '\n';
    }
    if (!use_changes.empty()) {
        // As emerge words them, for package.use.
        out << '\n' << paint("USE changes needed", Tone::heading) << '\n';
        for (const auto& row : use_changes) {
            for (std::size_t i = 4; i < row.size(); ++i) {
                out << paint(std::format("# required by {}", row.at(i)), Tone::note) << '\n';
            }
            out << paint(row.at(3), Tone::use) << '\n';
        }
        out << paint("emerge refuses the plan until package.use makes them", Tone::bad) << '\n';
    }
    if (!masked.empty()) {
        // As emerge warns of them: each package.mask comment once.
        out << '\n' << paint("Masked, installed", Tone::heading) << '\n';
        std::set<std::vector<std::string_view>> shown;
        for (const auto& row : masked) {
            out << paint(glyph.broken, Tone::bad) << ' ' << paint_cpv(row.at(0), paint)
                << paint(std::format("::{}", row.at(2)), Tone::repo) << "  "
                << paint("masked by", Tone::note) << ' ' << paint(row.at(3), Tone::bad) << '\n';
            const auto comment = row | std::views::drop(5) | std::ranges::to<std::vector>();
            if (comment.empty() || !shown.insert(comment).second) {
                continue;
            }
            out << "    " << paint(std::format("{}:", row.at(4)), Tone::note) << '\n';
            for (const auto line : comment) {
                out << "    " << paint(line, Tone::note) << '\n';
            }
        }
    }
    constexpr std::array<std::array<std::string_view, 2>, 12> nouns{
        {{" upgrade", " upgrades"},
         {" downgrade", " downgrades"},
         {" rebuild", " rebuilds"},
         {" new", " new"},
         {" in a new slot", " in new slots"},
         {" held", " held"},
         {" uninstall", " uninstalls"},
         {" blocker", " blockers"},
         {" unsatisfied", " unsatisfied"},
         {" unmet", " unmet"},
         {" USE change", " USE changes"},
         {" masked", " masked"}}};
    std::array<std::size_t, 12> all{};
    std::ranges::copy(counts, all.begin());
    all.at(5) += unlisted;
    all.at(6) = uninstalls.size();
    all.at(7) = blocks.size();
    all.at(8) = unsatisfied.size();
    all.at(9) = unmet.size();
    all.at(10) = use_changes.size();
    all.at(11) = masked.size();
    const bool counted = std::ranges::any_of(all, [](std::size_t count) { return count != 0; });
    if (counted) {
        out << '\n';
    }
    bool first = true;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all.at(i) == 0) {
            continue;
        }
        out << (first ? "" : paint(", ", Tone::note))
            << paint(std::to_string(all.at(i)), i >= 7 ? Tone::bad : Tone::count)
            << paint(nouns.at(i).at(all.at(i) == 1 ? 0 : 1), Tone::note);
        if (i == 5 && unlisted != 0) {
            const auto which = unlisted != all.at(i) ? std::format("the other {}", unlisted)
                               : unlisted == 1       ? std::string{"it"}
                                                     : std::string{"them"};
            out << paint(std::format(" (--held lists {})", which), Tone::note);
        }
        first = false;
    }
    out << (counted ? "\n" : "");
    if (trying) {
        if (!counted) {
            out << '\n';
        }
        std::string summary;
        const auto add = [&](std::size_t count, std::string_view one, std::string_view many) {
            if (count != 0) {
                summary += std::format("{}{} {}", summary.empty() ? "" : ", ", count,
                                       count == 1 ? one : many);
            }
        };
        add(more, "more merge", "more merges");
        add(dropped.size(), "fewer", "fewer");
        add(changed, "changed", "changed");
        add(for_use, "rebuilt for USE", "rebuilt for USE");
        out << paint("Tried:", Tone::heading) << ' '
            << paint(summary.empty() ? "the plan is the same" : summary, Tone::note) << '\n';
    }
    if (flags || fixed) {
        std::string legend = flags ? "flag* changed  flag% new in IUSE  (-flag%) gone from it" : "";
        if (fixed) {
            legend += std::format("{}(flag) set by the profile", legend.empty() ? "" : "  ");
        }
        out << '\n' << paint(legend, Tone::note) << '\n';
    }
}

void human_update_tree(std::ostream& out, std::span<const std::string> table,
                       std::span<const std::string> tree, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(table);
    // Table rows by the cpv a tree names them by: the installed one, or the new package.
    std::map<std::string_view, std::size_t, std::less<>> merges;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        merges.emplace(rows.at(i).at(2), i);
    }
    struct TreeNode {
        std::string_view label;
        std::vector<std::size_t> children;
    };
    // Node 0 holds the roots; children keep the order the merges first reach them in.
    std::vector<TreeNode> nodes(1);
    const auto child = [&nodes](std::size_t parent, std::string_view label) {
        for (const auto index : nodes.at(parent).children) {
            if (nodes.at(index).label == label) {
                return index;
            }
        }
        nodes.push_back({.label = label, .children = {}});
        nodes.at(parent).children.push_back(nodes.size() - 1);
        return nodes.size() - 1;
    };
    for (const auto& fields : split_all(tree)) {
        std::size_t at = 0;
        for (std::size_t i = 1; i < fields.size(); ++i) {
            at = child(at, fields.at(i));
        }
    }
    const auto put_merge = [&](const Fields& row) {
        const bool up = row.at(3) == "upgrade";
        const bool down = row.at(3) == "downgrade";
        const bool added = row.at(3) == "new" || row.at(3) == "new-slot";
        const auto mark = up      ? glyph.upgrade
                          : down  ? glyph.downgrade
                          : added ? glyph.added
                                  : glyph.rebuild;
        const auto tone = up || added ? Tone::good : down ? Tone::bad : Tone::use;
        const auto old = split_cpv(row.at(2));
        out << paint(mark, tone) << ' '
            << paint_cpv(row.at(2).substr(0, old.category.size() + 1 + old.name.size()), paint)
            << "  " << paint(old.version, Tone::version);
        if (row.at(2) != row.at(4)) {
            out << ' ' << paint(glyph.instead, Tone::note) << ' '
                << paint(split_cpv(row.at(4)).version, down ? Tone::bad : Tone::good);
        }
        out << "  " << paint("::" + std::string{row.at(5)}, Tone::repo) << "  "
            << paint(row.at(0), Tone::count);
        if (const auto waits = earlier_places(row.at(0), row.at(1)); !waits.empty()) {
            out << "  " << paint(glyph.waiting, Tone::note) << ' ' << paint(waits, Tone::count);
        }
        if (row.size() > 8) {
            put_beside(out, row.at(8), paint);
        }
    };
    const auto walk = [&](this const auto& self, std::size_t index,
                          const std::string& prefix) -> void {
        const auto& children = nodes.at(index).children;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const bool last = i + 1 == children.size();
            const auto& node = nodes.at(children.at(i));
            out << paint(prefix, Tone::note) << paint(last ? glyph.branch : glyph.tee, Tone::note)
                << ' ';
            if (const auto merge = merges.find(node.label); merge != merges.end()) {
                put_merge(rows.at(merge->second));
            } else {
                out << paint_cpv(node.label, paint);
            }
            out << '\n';
            self(children.at(i),
                 prefix + std::string(last ? "   " : std::string(glyph.rail) + " "));
        }
    };
    for (const auto root : nodes.front().children) {
        const auto set = nodes.at(root).label;
        if (set.empty()) {
            out << paint(glyph.orphan, Tone::bad) << ' ' << paint("nothing keeps", Tone::bad);
        } else {
            out << paint(set_glyph(set, glyph), Tone::root) << ' ' << paint(set, Tone::root);
        }
        out << '\n';
        walk(root, "");
    }
    std::array<std::size_t, 5> counts{};
    for (const auto& row : rows) {
        const auto kind = row.at(3);
        ++counts.at(kind == "upgrade"     ? 0
                    : kind == "downgrade" ? 1
                    : kind == "rebuild"   ? 2
                    : kind == "new"       ? 3
                                          : 4);
    }
    constexpr std::array<std::array<std::string_view, 2>, 5> nouns{
        {{" upgrade", " upgrades"},
         {" downgrade", " downgrades"},
         {" rebuild", " rebuilds"},
         {" new", " new"},
         {" in a new slot", " in new slots"}}};
    out << '\n';
    bool first = true;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts.at(i) == 0) {
            continue;
        }
        out << (first ? "" : paint(", ", Tone::note))
            << paint(std::to_string(counts.at(i)), Tone::count)
            << paint(nouns.at(i).at(counts.at(i) == 1 ? 0 : 1), Tone::note);
        first = false;
    }
    out << '\n';
}

void human_blockers(std::ostream& out, std::span<const std::string> records, bool named,
                    const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    if (rows.empty()) {
        out << paint(glyph.good, Tone::good) << ' '
            << paint(named ? "None of them holds a blocker or is blocked."
                           : "No installed package blocks another.",
                     Tone::good)
            << '\n';
        return;
    }
    std::size_t blocking = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        if (i == 0 || rows.at(i - 1).at(0) != row.at(0)) {
            out << (i == 0 ? "" : "\n") << paint(glyph.package, Tone::note) << ' '
                << paint_cpv(row.at(0), paint) << '\n';
        }
        out << "  " << kind_letter(row.at(1), paint) << "  " << paint(row.at(2), Tone::bad);
        if (row.at(3).empty()) {
            out << "  " << paint("blocks nothing installed", Tone::note);
        } else {
            out << "  " << paint("blocks", Tone::note) << ' ' << paint_cpv(row.at(3), paint);
            ++blocking;
        }
        out << '\n';
    }
    if (blocking > 0) {
        out << '\n'
            << paint(std::to_string(blocking), Tone::count)
            << paint(blocking == 1 ? " installed package blocked" : " installed packages blocked",
                     Tone::note)
            << '\n';
    }
    human_legend(out, theme);
}

void human_broken(std::ostream& out, std::span<const std::string> broken,
                  std::span<const std::string> replaced, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    // What is installed in a dependency's place, from the record's fourth field.
    const auto instead = [&](const Fields& row) {
        std::string text;
        if (row.size() > 3) {
            text += "  " + paint(glyph.instead, Tone::note);
            for (const auto cpv : std::views::split(row.at(3), ' ')) {
                text += ' ' + paint_cpv(std::string_view{cpv}, paint);
            }
        }
        return text;
    };
    const auto rows = split_all(broken);
    if (rows.empty()) {
        out << paint(glyph.good, Tone::good) << ' '
            << paint(replaced.empty() ? "Every dependency is satisfied." : "Nothing is broken.",
                     Tone::good)
            << '\n';
    }
    std::size_t packages = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        if (i == 0 || rows.at(i - 1).at(0) != row.at(0)) {
            out << (i == 0 ? "" : "\n") << paint(glyph.broken, Tone::bad) << ' '
                << paint_cpv(row.at(0), paint) << '\n';
            ++packages;
        }
        out << "  " << kind_letter(row.at(1), paint) << "  " << paint_dependency(row.at(2), paint)
            << instead(row) << '\n';
    }
    if (!rows.empty()) {
        out << '\n'
            << paint(std::to_string(rows.size()), Tone::count)
            << paint(rows.size() == 1 ? " unsatisfied dependency in "
                                      : " unsatisfied dependencies in ",
                     Tone::note)
            << paint(std::to_string(packages), Tone::count)
            << paint(packages == 1 ? " package" : " packages", Tone::note) << '\n';
    }
    // Build-time dependencies since replaced: what each package was built with, quietly.
    const auto old = split_all(replaced);
    if (!old.empty()) {
        std::size_t width = 0;
        for (const auto& row : old) {
            width = std::max(width, row.at(0).size());
        }
        out << '\n'
            << paint("Built with, since replaced", Tone::heading) << "  "
            << paint(std::to_string(old.size()), Tone::count) << '\n';
        for (const auto& row : old) {
            out << "  " << paint_cpv(row.at(0), paint) << spaces(row.at(0).size(), width) << "  "
                << kind_letter(row.at(1), paint) << "  " << paint(row.at(2), Tone::note)
                << instead(row) << '\n';
        }
    }
    if (!rows.empty() || !old.empty()) {
        human_legend(out, theme);
    }
}

void human_soname(std::ostream& out, std::span<const std::string> records, std::string_view soname,
                  bool providers, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    out << paint(glyph.soname, Tone::heading) << ' ' << paint(soname, Tone::heading) << "  ";
    if (records.empty()) {
        out << paint(providers ? "nothing installed provides it" : "nothing installed uses it",
                     Tone::note)
            << '\n';
        return;
    }
    out << paint(std::format("{} {}", providers ? "provided by" : "used by",
                             count(records.size(), "package", "packages")),
                 Tone::note)
        << '\n';
    auto rows = split_all(records);
    std::ranges::stable_sort(rows, {}, [](const Fields& row) { return row.at(1); });
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        if (i == 0 || rows.at(i - 1).at(1) != row.at(1)) {
            out << "  " << paint(row.at(1).empty() ? "no multilib category" : row.at(1), Tone::slot)
                << '\n';
        }
        out << "    " << paint(glyph.package, Tone::note) << ' ' << paint_cpv(row.at(0), paint)
            << '\n';
    }
}

void human_versions(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    const auto parts = [](const Fields& row) {
        const auto cpv = row.at(0).substr(0, row.at(0).find("::"));
        return std::pair{split_cpv(cpv), row.at(0).substr(cpv.size() + 2)};
    };
    for (std::size_t first = 0; first < rows.size();) {
        const auto cpv = parts(rows.at(first)).first;
        auto last = first;
        std::size_t version_width = 0;
        std::size_t slot_width = 0;
        while (last < rows.size() && parts(rows.at(last)).first.name == cpv.name &&
               parts(rows.at(last)).first.category == cpv.category) {
            version_width = std::max(version_width, parts(rows.at(last)).first.version.size());
            slot_width = std::max(slot_width, rows.at(last).at(1).size());
            ++last;
        }
        out << paint(theme.glyph().package, Tone::heading) << ' '
            << paint(cpv.category, Tone::category) << paint("/", Tone::note)
            << paint(cpv.name, Tone::name) << '\n';
        for (auto i = first; i < last; ++i) {
            const auto& row = rows.at(i);
            const auto [split, repo] = parts(row);
            const bool visible = row.at(2) == "visible";
            out << "    " << paint(split.version, visible ? Tone::version : Tone::bad)
                << std::string(version_width - split.version.size(), ' ') << "  "
                << paint(":", Tone::note) << paint(row.at(1), Tone::slot)
                << std::string(slot_width - row.at(1).size(), ' ') << "  "
                << paint("::", Tone::note) << paint(repo, Tone::repo);
            if (!visible) {
                std::string reasons;
                for (std::size_t field = 3; field < row.size(); ++field) {
                    reasons +=
                        std::string{reasons.empty() ? "" : ", "} + std::string{row.at(field)};
                }
                out << "  " << paint(reasons.empty() ? "masked" : "masked: " + reasons, Tone::note);
            }
            out << '\n';
        }
        first = last;
    }
}

void human_findings(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    if (records.empty()) {
        out << paint("no findings", Tone::note) << '\n';
        return;
    }
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t notes = 0;
    for (const auto& row : split_all(records)) {
        const auto file = row.at(0);
        const auto line = row.at(1);
        const auto severity = row.at(2);
        const auto atom = row.at(4);
        const auto token = row.at(5);
        const auto message = row.at(6);
        const auto tone = severity == "error"     ? Tone::bad
                          : severity == "warning" ? Tone::count
                                                  : Tone::note;
        (severity == "error" ? errors : severity == "warning" ? warnings : notes) += 1;
        const auto place = line == "0" ? std::string{file} : std::format("{}:{}", file, line);
        out << paint(place + ":", Tone::note) << ' ' << paint(std::string{severity} + ":", tone)
            << ' ' << paint(atom, Tone::name);
        if (!token.empty()) {
            out << ' ' << paint(token, Tone::use) << ':';
        }
        out << ' ' << message << '\n';
    }
    std::string summary;
    const auto count = [&](std::size_t n, std::string_view one) {
        if (n > 0) {
            summary +=
                std::format("{}{} {}{}", summary.empty() ? "" : ", ", n, one, n == 1 ? "" : "s");
        }
    };
    count(errors, "error");
    count(warnings, "warning");
    count(notes, "note");
    out << '\n' << paint(summary, Tone::count) << '\n';
}

void human_search(std::ostream& out, std::span<const std::string> records,
                  std::span<const std::string> keys, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    bool first = true;
    for (const auto& key : keys) {
        std::vector<const Fields*> found;
        for (const auto& row : rows) {
            if (row.at(0) == key) {
                found.push_back(&row);
            }
        }
        out << (first ? "" : "\n") << paint(glyph.search, Tone::heading) << ' '
            << paint(key, Tone::heading) << "  "
            << paint(found.empty() ? std::string{"nothing found"}
                                   : count(found.size(), "package", "packages"),
                     Tone::note)
            << '\n';
        first = false;
        for (const auto* row : found) {
            const auto field = [row](std::size_t i) { return row->at(i); };
            const bool masked = field(3) == "masked";
            out << "  " << paint(glyph.package, Tone::note) << ' ' << paint_cpv(field(1), paint)
                << '\n';
            const auto detail = [&](std::string_view label, const std::string& value) {
                out << "      " << paint(std::format("{:<11}", label), Tone::note) << value << '\n';
            };
            if (!field(2).empty()) {
                detail("available", paint(field(2), masked ? Tone::bad : Tone::version) +
                                        (masked ? " " + paint("masked", Tone::bad) : ""));
            }
            detail("installed", field(4).empty() ? paint("not installed", Tone::note)
                                                 : paint(field(4), Tone::version));
            if (!field(5).empty()) {
                detail("homepage", std::string{field(5)});
            }
            if (!field(6).empty()) {
                detail("license", std::string{field(6)});
            }
            if (!field(7).empty()) {
                out << "      " << field(7) << '\n';
            }
        }
    }
}

namespace {

bool is_atom_change(const Fields& row) {
    return row.at(1) == "added" || row.at(1) == "removed";
}

// difference_lines' package rows, each after its prefix (none without prefixes), their columns
// aligned; how many of each kind: upgrade, downgrade, rebuild, new, uninstall.
std::array<std::size_t, 5> put_changes(std::ostream& out, const std::vector<Fields>& rows,
                                       const std::vector<std::string_view>& prefixes,
                                       const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    // A package row's cp and its versions before and after.
    const auto parts = [](const Fields& row) {
        const auto cpv = row.at(0).empty() ? row.at(2) : row.at(0);
        const auto split = split_cpv(cpv);
        const auto cp = cpv.substr(0, split.category.size() + 1 + split.name.size());
        const auto version = [&cp](std::string_view of) {
            return of.empty() ? of : of.substr(cp.size() + 1);
        };
        return std::tuple{cp, version(row.at(0)), version(row.at(2))};
    };
    std::size_t cp_width = 0;
    std::size_t version_width = 0;
    std::size_t move_width = 0;
    for (const auto& row : rows) {
        if (is_atom_change(row)) {
            continue;
        }
        const auto [cp, from, to] = parts(row);
        cp_width = std::max(cp_width, cp.size());
        version_width = std::max(version_width, from.size());
        if (row.at(1) != "rebuild" && !to.empty()) {
            move_width = std::max(move_width, 3 + to.size());
        }
    }
    std::array<std::size_t, 5> counts{};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        if (is_atom_change(row)) {
            continue;
        }
        const auto kind = row.at(1);
        const auto index = kind == "upgrade"     ? 0U
                           : kind == "downgrade" ? 1U
                           : kind == "rebuild"   ? 2U
                           : kind == "new"       ? 3U
                                                 : 4U;
        ++counts.at(index);
        constexpr std::array<Tone, 5> tones{Tone::good, Tone::bad, Tone::use, Tone::good,
                                            Tone::bad};
        const std::array<std::string_view, 5> marks{glyph.upgrade, glyph.downgrade, glyph.rebuild,
                                                    glyph.added, glyph.orphan};
        const auto [cp, from, to] = parts(row);
        if (!prefixes.empty()) {
            out << paint(prefixes.at(i), Tone::note) << "  ";
        }
        out << paint(marks.at(index), tones.at(index)) << ' ' << paint_cpv(cp, paint);
        const auto flags = row.size() > 3 ? row.at(3) : std::string_view{};
        // The padding owed so far, written only when something follows it.
        std::size_t pad = cp_width - cp.size() + 2;
        if (!from.empty()) {
            out << std::string(pad, ' ') << paint(from, Tone::version);
            pad = 0;
        }
        pad += version_width - from.size();
        if (index != 2 && !to.empty()) {
            out << std::string(pad, ' ') << ' ' << paint(glyph.instead, Tone::note) << ' '
                << paint(to, index == 1 ? Tone::bad : Tone::good);
            pad = move_width - (3 + to.size());
        } else {
            pad += move_width;
        }
        if (!flags.empty()) {
            out << std::string(pad, ' ') << ' ';
            for (const auto flag : std::views::split(flags, ' ')) {
                const std::string_view text{flag};
                out << ' ' << paint(text, text.starts_with('+') ? Tone::use : Tone::note);
            }
        }
        out << '\n';
    }
    return counts;
}

} // namespace

void human_history(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    if (records.empty()) {
        out << theme.paint(theme.glyph().good, Tone::good) << ' '
            << theme.paint("Nothing in the history.", Tone::good) << '\n';
        return;
    }
    std::vector<std::string_view> times;
    std::vector<Fields> rows;
    for (auto row : split_all(records)) {
        times.push_back(row.front());
        row.erase(row.begin());
        rows.push_back(std::move(row));
    }
    std::ignore = put_changes(out, rows, times, theme);
}

void human_diff(std::ostream& out, std::span<const std::string> records, std::string_view since,
                const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    if (rows.empty()) {
        out << paint(glyph.good, Tone::good) << ' '
            << paint(std::format("Nothing changed since {}.", since), Tone::good) << '\n';
        return;
    }
    out << paint(std::format("Since {}", since), Tone::heading) << '\n';
    const auto is_atom = is_atom_change;
    std::size_t set_width = 0;
    for (const auto& row : rows) {
        if (is_atom(row)) {
            set_width = std::max(set_width, row.at(0).size());
        }
    }
    const auto counts = put_changes(out, rows, {}, theme);
    // A line per set: the atoms it gained, then those it lost.
    for (std::size_t i = 0; i < rows.size();) {
        if (!is_atom(rows.at(i))) {
            ++i;
            continue;
        }
        const auto set = rows.at(i).at(0);
        out << paint(set_glyph(set, glyph), Tone::root) << ' ' << paint(set, Tone::root)
            << spaces(set.size(), set_width);
        for (; i < rows.size() && is_atom(rows.at(i)) && rows.at(i).at(0) == set; ++i) {
            const bool added = rows.at(i).at(1) == "added";
            out << "  " << paint(added ? "+" : "-", added ? Tone::good : Tone::bad)
                << paint(rows.at(i).at(2), added ? Tone::version : Tone::note);
        }
        out << '\n';
    }
    if (std::ranges::all_of(counts, [](std::size_t count) { return count == 0; })) {
        return;
    }
    constexpr std::array<std::array<std::string_view, 2>, 5> nouns{
        {{" upgrade", " upgrades"},
         {" downgrade", " downgrades"},
         {" rebuild", " rebuilds"},
         {" new", " new"},
         {" uninstalled", " uninstalled"}}};
    out << '\n';
    bool first = true;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts.at(i) == 0) {
            continue;
        }
        out << (first ? "" : paint(", ", Tone::note))
            << paint(std::to_string(counts.at(i)), Tone::count)
            << paint(nouns.at(i).at(counts.at(i) == 1 ? 0 : 1), Tone::note);
        first = false;
    }
    out << '\n';
}

namespace {

std::string lower(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

// A repository's file from its name on: /var/db/repos/gentoo/profiles/x as gentoo/profiles/x.
std::string_view short_place(std::string_view place) {
    const auto profiles = place.find("/profiles/");
    if (profiles == std::string_view::npos || profiles == 0) {
        return place;
    }
    return place.substr(place.rfind('/', profiles - 1) + 1);
}

// The group of groups a flag's prefix names, or none.
std::optional<std::string_view> group_of(std::string_view flag,
                                         std::span<const std::string> groups) {
    for (const auto& group : groups) {
        if (flag.starts_with(lower(group) + "_")) {
            return group;
        }
    }
    return std::nullopt;
}

// Where a step was set, in words: its file and line, or the layer of a package's own.
std::string use_place(std::string_view layer, std::string_view place, std::string_view token) {
    std::string where;
    if (!place.empty()) {
        where = std::string{short_place(place)};
    } else if (layer == "pkginternal") {
        where = token == "-test" ? "RESTRICT=test" : "IUSE default";
    } else if (layer == "features") {
        where = "FEATURES=test";
    } else if (layer == "arch") {
        where = "ARCH";
    } else if (layer == "env") {
        where = "the environment";
    } else if (layer.empty()) {
        where = "not set";
    } else {
        where = std::string{layer};
    }
    const bool undone = token.starts_with('-');
    if (layer == "force") {
        return (undone ? "unforced  " : "forced  ") + where;
    }
    if (layer == "mask") {
        return (undone ? "unmasked  " : "masked  ") + where;
    }
    return where;
}

// What a token did to flag where it does not name it: a wildcard, or a USE_EXPAND variable.
std::string use_token_note(std::string_view flag, std::string_view token,
                           std::span<const std::string> groups) {
    if (token.empty() || token == flag || (token.starts_with('-') && token.substr(1) == flag)) {
        return {};
    }
    if (std::ranges::contains(groups, token)) {
        return std::format("({}=)", token);
    }
    if (token.starts_with('-') && token.ends_with("_*")) {
        const auto prefix = token.substr(1, token.size() - 2);
        for (const auto& group : groups) {
            if (lower(group) + "_" == prefix) {
                return std::format("({}: -*)", group);
            }
        }
    }
    if (token == "-*" || token.ends_with("_*")) {
        return std::format("({})", token);
    }
    if (const auto group = group_of(flag, groups)) {
        return std::format("({})", *group);
    }
    return std::format("({})", token);
}

bool continues_code_point(char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

std::size_t columns_of(std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return !continues_code_point(c); }));
}

// Footnote number's mark: its digits raised where the glyphs have them, otherwise [number].
std::string footnote_mark(std::size_t number, const Glyphs& glyph) {
    const auto digits = std::to_string(number);
    if (glyph.superscripts.empty()) {
        return std::format("[{}]", digits);
    }
    std::string mark;
    for (const char digit : digits) {
        // The digit's code point among the ten.
        auto wanted = static_cast<std::size_t>(digit - '0');
        std::size_t start = 0;
        while (start < glyph.superscripts.size()) {
            std::size_t end = start + 1;
            while (end < glyph.superscripts.size() &&
                   continues_code_point(glyph.superscripts.at(end))) {
                ++end;
            }
            if (wanted == 0) {
                mark += glyph.superscripts.substr(start, end - start);
                break;
            }
            --wanted;
            start = end;
        }
    }
    return mark;
}

// Items after a two-space indent, separated by separator, wrapped before 100 columns. Each item
// is its text, painted, and its width.
void wrapped(std::ostream& out, std::span<const std::pair<std::string, std::size_t>> items,
             std::string_view separator) {
    std::size_t column = 2;
    out << "  ";
    bool first = true;
    for (const auto& [text, width] : items) {
        if (!first && column + separator.size() + width > 100) {
            out << "\n  ";
            column = 2;
        } else if (!first) {
            out << separator;
            column += separator.size();
        }
        out << text;
        column += width;
        first = false;
    }
    out << '\n';
}

void use_header(std::ostream& out, std::string_view key, const Painter& paint) {
    const auto colons = std::min(key.find("::"), key.size());
    out << paint_cpv(key.substr(0, colons), paint) << paint(key.substr(colons), Tone::repo);
}

} // namespace

void human_use(std::ostream& out, std::span<const std::string> records,
               std::span<const UseVersion> versions, std::span<const std::string> groups,
               const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    std::map<std::string_view, std::vector<const Fields*>> rows_of;
    for (const auto& row : rows) {
        rows_of[row.at(0)].push_back(&row);
    }
    const auto has_rows = [&](const UseVersion& version) { return rows_of.contains(version.key); };
    // Packages in the order of their first version with flags.
    std::vector<std::string_view> cps;
    for (const auto& version : versions) {
        if (!std::ranges::contains(cps, version.cp) && has_rows(version)) {
            cps.push_back(version.cp);
        }
    }
    bool first = true;
    for (const auto cp : cps) {
        out << (first ? "" : "\n");
        first = false;
        std::vector<const UseVersion*> shown;
        for (const auto& version : versions) {
            if (version.cp == cp && has_rows(version)) {
                shown.push_back(&version);
            }
        }
        const auto repo_of = [](const UseVersion& version) {
            const std::string_view key = version.key;
            return key.substr(std::min(key.find("::"), key.size()));
        };
        const bool one_repo = std::ranges::all_of(shown, [&](const UseVersion* version) {
            return repo_of(*version) == repo_of(*shown.front());
        });
        const auto label = [&](const UseVersion& version) {
            const std::string_view key = version.key;
            const auto repo = repo_of(version);
            auto text = std::string{key.substr(std::min(cp.size() + 1, key.size() - repo.size()))};
            text = text.substr(0, text.size() - repo.size());
            return one_repo ? text : text + std::string{repo};
        };
        if (shown.size() == 1) {
            use_header(out, shown.front()->key, paint);
            out << '\n';
        } else {
            out << paint_cpv(cp, paint)
                << (one_repo ? paint(repo_of(*shown.front()), Tone::repo) : "") << '\n';
            std::vector<std::pair<std::string, std::size_t>> items;
            bool installed = false;
            bool pick = false;
            for (const auto* version : shown) {
                const auto text = label(*version);
                const auto mark = version->installed ? glyph.installed
                                  : version->pick    ? glyph.pick
                                                     : std::string_view{};
                // A version both installed and picked shows both.
                const auto also =
                    version->installed && version->pick ? glyph.pick : std::string_view{};
                installed = installed || version->installed;
                pick = pick || version->pick;
                items.emplace_back(paint(text, Tone::version) + paint(mark, Tone::good) +
                                       paint(also, Tone::good),
                                   columns_of(text) + columns_of(mark) + columns_of(also));
            }
            wrapped(out, items, "  ");
            if (installed || pick) {
                out << "  ";
                if (installed) {
                    out << paint(glyph.installed, Tone::good) << paint(" installed", Tone::note);
                }
                if (pick) {
                    out << (installed ? "  " : "") << paint(glyph.pick, Tone::good)
                        << paint(" emerge's pick", Tone::note);
                }
                out << '\n';
            }
            out << '\n';
        }
        // Each flag as one or more of the versions have it alike: spelled and set the same.
        struct Merged {
            const Fields* row = nullptr;
            std::string bare;
            std::vector<bool> in;
        };
        std::vector<Merged> merged;
        // Each merged row's position, by its fields after the key.
        std::map<std::vector<std::string_view>, std::size_t> merged_at;
        for (std::size_t at = 0; at < shown.size(); ++at) {
            for (const auto* row : rows_of.at(shown.at(at)->key)) {
                if (row->at(2) != "iuse") {
                    continue;
                }
                const std::vector<std::string_view> fields{row->at(1), row->at(3), row->at(4),
                                                           row->at(5)};
                const auto [position, added] = merged_at.try_emplace(fields, merged.size());
                if (added) {
                    std::string_view bare = row->at(1);
                    if (bare.starts_with('(')) {
                        bare = bare.substr(1, bare.size() - 2);
                    }
                    if (bare.starts_with('-')) {
                        bare.remove_prefix(1);
                    }
                    merged.push_back({.row = row,
                                      .bare = std::string{bare},
                                      .in = std::vector<bool>(shown.size())});
                }
                merged.at(position->second).in.at(at) = true;
            }
        }
        if (shown.size() > 1) {
            std::ranges::stable_sort(merged, {}, &Merged::bare);
        }
        // The versions in a set, by whole slots where two or more versions fill one.
        const auto items_of = [&](const std::vector<bool>& set) {
            std::vector<std::string> items;
            std::vector<bool> covered(shown.size());
            for (std::size_t i = 0; i < shown.size(); ++i) {
                if (!set.at(i) || covered.at(i)) {
                    continue;
                }
                const auto& slot = shown.at(i)->slot;
                std::size_t members = 0;
                bool whole = true;
                for (std::size_t j = 0; j < shown.size(); ++j) {
                    if (shown.at(j)->slot == slot) {
                        ++members;
                        whole = whole && set.at(j);
                    }
                }
                if (members >= 2 && whole) {
                    items.push_back(":" + slot);
                    for (std::size_t j = 0; j < shown.size(); ++j) {
                        covered.at(j) = covered.at(j) || shown.at(j)->slot == slot;
                    }
                } else {
                    items.push_back(label(*shown.at(i)));
                    covered.at(i) = true;
                }
            }
            return items;
        };
        const auto joined = [](const std::vector<std::string>& items) {
            std::string text;
            for (const auto& item : items) {
                text += (text.empty() ? "" : ", ") + item;
            }
            return text;
        };
        // The fewer words of the versions that have a flag and of those that do not.
        const auto describe = [&](const std::vector<bool>& set) {
            std::vector<bool> others(set.size());
            for (std::size_t i = 0; i < set.size(); ++i) {
                others.at(i) = !set.at(i);
            }
            const auto having = items_of(set);
            const auto lacking = items_of(others);
            if (lacking.size() < having.size()) {
                return "not " + joined(lacking);
            }
            return having.size() == 1 ? having.front() + " only" : joined(having);
        };
        // Plain flags first, then each group's, named without its prefix.
        struct Shown {
            std::string name;
            std::string mark;
            bool on = false;
            const Fields* row = nullptr;
            const std::vector<bool>* in = nullptr;
        };
        std::vector<std::pair<std::string, std::vector<Shown>>> sections{{"", {}}};
        for (const auto& group : groups) {
            sections.emplace_back(group, std::vector<Shown>{});
        }
        for (const auto& each : merged) {
            const auto& row = *each.row;
            const auto spelled = row.at(1);
            const bool fixed = spelled.starts_with('(');
            const bool on = !spelled.substr(fixed ? 1 : 0).starts_with('-');
            const auto group = group_of(each.bare, groups);
            auto name = std::string{group ? std::string_view{each.bare}.substr(group->size() + 1)
                                          : std::string_view{each.bare}};
            name = std::format("{}{}{}{}", fixed ? "(" : "", on ? "" : "-", name, fixed ? ")" : "");
            const auto section = std::ranges::find_if(
                sections, [&](const auto& s) { return s.first == group.value_or(""); });
            section->second.push_back(
                {.name = std::move(name), .mark = {}, .on = on, .row = &row, .in = &each.in});
        }
        // Footnotes numbered in the order shown, one per set of versions.
        std::vector<std::vector<bool>> notes;
        for (auto& [group, flags] : sections) {
            for (auto& flag : flags) {
                const auto& in = *flag.in;
                if (std::ranges::all_of(in, std::identity{})) {
                    continue;
                }
                auto note = std::ranges::find(notes, in);
                if (note == notes.end()) {
                    notes.push_back(in);
                    note = std::prev(notes.end());
                }
                flag.mark =
                    footnote_mark(static_cast<std::size_t>(note - notes.begin()) + 1, glyph);
            }
        }
        const auto width_of = [](const Shown& flag) {
            return flag.name.size() + (flag.mark.empty() ? 0 : 1 + columns_of(flag.mark));
        };
        std::size_t width = 0;
        for (const auto& [group, flags] : sections) {
            for (const auto& flag : flags) {
                width = std::max(width, width_of(flag) + (group.empty() ? 0 : 2));
            }
        }
        for (const auto& [group, flags] : sections) {
            if (flags.empty()) {
                continue;
            }
            const std::string indent = group.empty() ? "  " : "    ";
            if (!group.empty()) {
                out << "  " << paint(group, Tone::heading) << '\n';
            }
            for (const auto& flag : flags) {
                const auto& row = *flag.row;
                out << indent << paint(flag.name, flag.on ? Tone::use : Tone::note)
                    << (flag.mark.empty() ? "" : " " + paint(flag.mark, Tone::count))
                    << std::string(width + 2 - width_of(flag) - (indent.size() - 2), ' ')
                    << use_place(row.at(3), row.at(4), row.at(5));
                std::string_view spelled = row.at(1);
                if (spelled.starts_with('(')) {
                    spelled = spelled.substr(1, spelled.size() - 2);
                }
                if (spelled.starts_with('-')) {
                    spelled.remove_prefix(1);
                }
                if (const auto note = use_token_note(spelled, row.at(5), groups); !note.empty()) {
                    out << "  " << paint(note, Tone::note);
                }
                out << '\n';
            }
        }
        if (!notes.empty()) {
            out << '\n';
            std::vector<std::pair<std::string, std::size_t>> items;
            for (std::size_t i = 0; i < notes.size(); ++i) {
                const auto mark = footnote_mark(i + 1, glyph);
                const auto text = describe(notes.at(i));
                items.emplace_back(paint(mark, Tone::count) + " " + paint(text, Tone::note),
                                   columns_of(mark) + 1 + columns_of(text));
            }
            wrapped(out, items, "   ");
        }
    }
}

void human_use_steps(std::ostream& out, std::span<const std::string> records,
                     std::span<const std::string> groups, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    std::string_view current;
    const Fields* previous = nullptr;
    for (const auto& row : rows) {
        if (row.at(0) != current) {
            out << (current.empty() ? "" : "\n");
            current = row.at(0);
            previous = nullptr;
            use_header(out, current, paint);
            out << "  " << paint(row.at(1), Tone::use) << '\n';
        }
        const bool on = row.at(2) == "on";
        out << "  " << paint(on ? "+" : "-", on ? Tone::good : Tone::bad) << ' '
            << use_place(row.at(3), row.at(4), row.at(6));
        if (!row.at(5).empty()) {
            out << "  " << paint_dependency(row.at(5), paint);
        }
        // Package entries apply least specific first, so a later one of the same layer outranks.
        if (previous != nullptr && !row.at(5).empty() && !previous->at(5).empty() &&
            previous->at(3) == row.at(3) && previous->at(5) != row.at(5)) {
            out << "  " << paint("(more specific)", Tone::note);
        }
        if (const auto note = use_token_note(row.at(1), row.at(6), groups); !note.empty()) {
            out << "  " << paint(note, Tone::note);
        }
        if (row.at(7) == "unchanged") {
            out << "  " << paint("(no change)", Tone::note);
        }
        out << '\n';
        previous = &row;
    }
}

void human_match(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> atoms, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto rows = split_all(records);
    bool first = true;
    for (const auto& atom : atoms) {
        std::vector<std::string_view> found;
        for (const auto& row : rows) {
            if (row.at(0) == atom) {
                found.push_back(row.at(1));
            }
        }
        out << (first ? "" : "\n") << paint(theme.glyph().search, Tone::heading) << ' '
            << paint_dependency(atom, paint) << "  "
            << paint(found.empty() ? "no installed package"
                                   : count(found.size(), "match", "matches"),
                     Tone::note)
            << '\n';
        first = false;
        for (const auto cpv : found) {
            out << "  " << paint(theme.glyph().package, Tone::note) << ' ' << paint_cpv(cpv, paint)
                << '\n';
        }
    }
}

} // namespace egraph
