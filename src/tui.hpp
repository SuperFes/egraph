#pragma once

// The terminal interface. The app is a template over its screen, so tests drive it with a fake
// one; only tui.cpp pairs it with the Notcurses Screen.

#include "cli.hpp"
#include "human.hpp"
#include "screen.hpp"
#include "store.hpp"

#include <cstddef>
#include <format>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace egraph::tui {

// Built with Notcurses; without it, open_and_run only reports that.
[[nodiscard]] bool available();

// Catppuccin Mocha, as the human layout uses.
namespace palette {
inline constexpr Color mantle{.red = 24, .green = 24, .blue = 37};
inline constexpr Color text{.red = 205, .green = 214, .blue = 244};
inline constexpr Color subtext{.red = 166, .green = 173, .blue = 200};
inline constexpr Color overlay{.red = 127, .green = 132, .blue = 156};
inline constexpr Color mauve{.red = 203, .green = 166, .blue = 247};
inline constexpr Color blue{.red = 137, .green = 180, .blue = 250};
inline constexpr Color peach{.red = 250, .green = 179, .blue = 135};
} // namespace palette

struct Summary {
    std::string eroot;
    std::size_t packages = 0;
    std::size_t edges = 0;
    std::size_t root_atoms = 0;
};

template <class S> void draw(S& screen, const Summary& summary, const Glyphs& glyph) {
    const auto size = screen.size();
    screen.clear();
    if (size.rows < 2) {
        screen.render();
        return;
    }
    const Pen bar{.fg = palette::text, .bg = palette::mantle};
    screen.fill_row(0, bar);
    const auto title = std::format(" {} egraph ", glyph.package);
    screen.put(0, 0, title, {.fg = palette::mauve, .bg = palette::mantle, .bold = true});
    screen.put(0, static_cast<unsigned>(title.size()), summary.eroot,
               {.fg = palette::subtext, .bg = palette::mantle, .italic = true});

    const std::vector<std::pair<std::string, std::size_t>> facts{
        {"packages", summary.packages},
        {"dependency edges", summary.edges},
        {"root atoms", summary.root_atoms},
    };
    unsigned row = 2;
    for (const auto& [label, value] : facts) {
        if (row + 1 >= size.rows) {
            break;
        }
        screen.put(row, 2, std::format("{:>8}", value),
                   {.fg = palette::peach, .bg = std::nullopt, .bold = true});
        screen.put(row, 12, label, {.fg = palette::blue, .bg = std::nullopt});
        ++row;
    }

    screen.fill_row(size.rows - 1, bar);
    screen.put(size.rows - 1, 1, "q", {.fg = palette::mauve, .bg = palette::mantle, .bold = true});
    screen.put(size.rows - 1, 3, "quit", {.fg = palette::overlay, .bg = palette::mantle});
    screen.render();
}

// Runs until the user quits or input ends.
template <class S> void run(S& screen, const Summary& summary, const Glyphs& glyph) {
    draw(screen, summary, glyph);
    while (true) {
        const auto key = screen.read();
        switch (key.kind) {
        case KeyKind::closed:
        case KeyKind::escape:
            return;
        case KeyKind::character:
            if (key.code == U'q' || key.code == U'Q') {
                return;
            }
            break;
        case KeyKind::resize:
            draw(screen, summary, glyph);
            break;
        default:
            break;
        }
    }
}

[[nodiscard]] Summary summarize(const Store& store);

// Opens the terminal and runs the interface over store; errors go to err.
[[nodiscard]] Exit open_and_run(const Store& store, GlyphSet glyphs, std::ostream& err);

} // namespace egraph::tui
