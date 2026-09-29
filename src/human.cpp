#include "human.hpp"

#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ostream>
#include <ranges>
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
    .trail = "\uE0B1",
    .cursor = "▌",
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
    .trail = "›",
    .cursor = "▌",
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
    .trail = ">",
    .cursor = ">",
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

void human_updates(std::ostream& out, std::span<const std::string> records, const Theme& theme) {
    const auto& paint = theme.paint;
    const auto& glyph = theme.glyph();
    const auto rows = split_all(records);
    const auto is_held = [](const auto& row) { return row.at(1) == "held"; };
    const auto is_new = [](const auto& row) { return row.at(1) == "new"; };
    // Every row shares the columns, so held ones line up under the updates.
    std::size_t cp_width = 0;
    std::size_t version_width = 0;
    // " > version" when any row moves to another version, so every repo lines up.
    std::size_t move_width = 0;
    for (const auto& row : rows) {
        const auto parts = split_cpv(row.at(0));
        cp_width = std::max(cp_width, parts.category.size() + 1 + parts.name.size());
        version_width = std::max(version_width, parts.version.size());
        if (row.at(0) != row.at(2)) {
            move_width = std::max(move_width, 3 + split_cpv(row.at(2)).version.size());
        }
    }
    bool flags = false;
    // The package, its version and where it goes, its repository, and any flags.
    const auto put_row = [&](const auto& row, std::string_view mark, Tone tone) {
        const auto old = split_cpv(row.at(0));
        const auto target = split_cpv(row.at(2));
        const auto cp = row.at(0).substr(0, old.category.size() + 1 + old.name.size());
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
        // A package, then its atom: what pulls a new one in, or what a rebuild is for.
        const auto put_why = [&](std::string_view by) {
            const auto cut = std::min(by.find(' '), by.size());
            out << "  " << paint(by.substr(0, cut), Tone::version) << ' '
                << paint(by.substr(std::min(cut + 1, by.size())), Tone::note);
        };
        if (row.at(1) == "new") {
            if (row.size() > 4) {
                put_why(row.at(4));
            }
        } else if (row.at(1) == "rebuild" && row.size() > 5) {
            put_why(row.at(5));
        } else if (row.size() > 4 && !row.at(4).empty()) {
            flags = true;
            out << ' ';
            for (const auto flag : std::views::split(row.at(4), ' ')) {
                const std::string_view text{flag};
                out << ' ' << paint(text, text.contains('*') ? Tone::use : Tone::note);
            }
        }
        out << '\n';
    };
    // up, down, rebuild, new, held
    std::array<std::size_t, 5> counts{};
    for (const auto& row : rows) {
        if (is_held(row)) {
            ++counts.at(4);
            continue;
        }
        if (is_new(row)) {
            ++counts.at(3);
            continue;
        }
        const bool up = row.at(1) == "upgrade";
        const bool down = row.at(1) == "downgrade";
        ++counts.at(up ? 0 : down ? 1 : 2);
        put_row(row,
                up     ? glyph.upgrade
                : down ? glyph.downgrade
                       : glyph.rebuild,
                up     ? Tone::good
                : down ? Tone::bad
                       : Tone::use);
    }
    if (counts.at(0) + counts.at(1) + counts.at(2) + counts.at(3) == 0) {
        out << paint(glyph.good, Tone::good) << ' ' << paint("Nothing to update.", Tone::good)
            << '\n';
        if (counts.at(4) == 0) {
            return;
        }
    }
    if (counts.at(3) != 0) {
        out << '\n' << paint("New", Tone::heading) << '\n';
        for (const auto& row : rows) {
            if (is_new(row)) {
                put_row(row, glyph.added, Tone::good);
            }
        }
    }
    if (counts.at(4) != 0) {
        out << '\n' << paint("Held back", Tone::heading) << '\n';
        for (const auto& row : rows) {
            if (!is_held(row)) {
                continue;
            }
            put_row(row, glyph.held, Tone::bad);
            // One line per dependent holding it: its cpv, then its atoms that do.
            std::size_t holder_width = 0;
            for (std::size_t i = 5; i < row.size(); ++i) {
                holder_width =
                    std::max(holder_width, row.at(i).substr(0, row.at(i).find(' ')).size());
            }
            for (std::size_t i = 5; i < row.size(); ++i) {
                const auto field = row.at(i);
                const auto holder = field.substr(0, field.find(' '));
                out << "    " << paint(holder, Tone::version)
                    << spaces(holder.size(), holder_width);
                if (holder.size() < field.size()) {
                    for (const auto atom :
                         std::views::split(field.substr(holder.size() + 1), ' ')) {
                        out << "  " << paint(std::string_view{atom}, Tone::note);
                    }
                }
                out << '\n';
            }
        }
    }
    constexpr std::array<std::array<std::string_view, 2>, 5> nouns{{{" upgrade", " upgrades"},
                                                                    {" downgrade", " downgrades"},
                                                                    {" rebuild", " rebuilds"},
                                                                    {" new", " new"},
                                                                    {" held", " held"}}};
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
    if (flags) {
        out << '\n'
            << paint("flag* changed  flag% new in IUSE  (-flag%) gone from it", Tone::note) << '\n';
    }
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
