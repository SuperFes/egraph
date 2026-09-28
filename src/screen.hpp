#pragma once

// A full-screen terminal, and the only place egraph calls Notcurses (in screen.cpp, which is
// built only with the tui feature). Like os.hpp, everything here is values.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct notcurses;

namespace egraph::tui {

struct Color {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

// How text is drawn; an unset colour is the terminal's own.
struct Pen {
    std::optional<Color> fg;
    std::optional<Color> bg;
    bool bold = false;
    bool italic = false;
};

enum class KeyKind : std::uint8_t {
    character,
    up,
    down,
    left,
    right,
    page_up,
    page_down,
    home,
    end,
    enter,
    tab,
    backspace,
    escape,
    // The terminal changed size; redraw.
    resize,
    // Input ended or failed.
    closed,
    other,
};

struct Key {
    KeyKind kind = KeyKind::other;
    // The character, for KeyKind::character.
    char32_t code = 0;
};

struct Size {
    unsigned rows = 0;
    unsigned cols = 0;
};

class Screen {
  public:
    // Takes over the terminal until the Screen is destroyed.
    static std::expected<Screen, std::string> open();

    [[nodiscard]] Size size() const;
    void clear();
    // Text at a cell, clipped at the right edge.
    void put(unsigned row, unsigned col, std::string_view text, const Pen& pen);
    // Paints a whole row in pen's background.
    void fill_row(unsigned row, const Pen& pen);
    void render();
    // Waits for the next key press.
    Key read();

  private:
    struct Stop {
        void operator()(notcurses* terminal) const;
    };
    explicit Screen(std::unique_ptr<notcurses, Stop> terminal) : terminal_(std::move(terminal)) {}

    std::unique_ptr<notcurses, Stop> terminal_;
};

} // namespace egraph::tui
