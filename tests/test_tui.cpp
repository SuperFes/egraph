#include "tui.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

using egraph::tui::Key;
using egraph::tui::KeyKind;

namespace {

// A grid of cells in place of a terminal, fed keys from a script.
class FakeScreen {
  public:
    FakeScreen(unsigned rows, unsigned cols, std::deque<Key> keys)
        : rows_(rows), cols_(cols), grid_(rows, std::string(cols, ' ')), keys_(std::move(keys)) {}

    [[nodiscard]] egraph::tui::Size size() const { return {.rows = rows_, .cols = cols_}; }
    void clear() { grid_.assign(rows_, std::string(cols_, ' ')); }
    void put(unsigned row, unsigned col, std::string_view text, const egraph::tui::Pen&) {
        if (row >= rows_ || col >= cols_) {
            return;
        }
        grid_.at(row).replace(col, std::min<std::size_t>(text.size(), cols_ - col),
                              text.substr(0, cols_ - col));
    }
    void fill_row(unsigned row, const egraph::tui::Pen& pen) {
        put(row, 0, std::string(cols_, ' '), pen);
    }
    void render() { ++renders; }
    Key read() {
        if (keys_.empty()) {
            return {.kind = KeyKind::closed};
        }
        const auto key = keys_.front();
        keys_.pop_front();
        return key;
    }

    [[nodiscard]] const std::vector<std::string>& grid() const { return grid_; }
    [[nodiscard]] bool keys_left() const { return !keys_.empty(); }
    int renders = 0;

  private:
    unsigned rows_;
    unsigned cols_;
    std::vector<std::string> grid_;
    std::deque<Key> keys_;
};

const egraph::tui::Summary summary{
    .eroot = "/", .packages = 2326, .edges = 34432, .root_atoms = 260};

Key character(char32_t code) {
    return {.kind = KeyKind::character, .code = code};
}

} // namespace

TEST_CASE("the interface shows the store and quits on q") {
    FakeScreen screen{8, 40, {character(U'x'), character(U'q'), character(U'z')}};
    egraph::tui::run(screen, summary, egraph::glyphs(egraph::GlyphSet::ascii));
    CHECK(screen.grid().at(0).starts_with(" * egraph /"));
    CHECK(screen.grid().at(2) == "      2326  packages" + std::string(20, ' '));
    CHECK(screen.grid().at(3).starts_with("     34432  dependency edges"));
    CHECK(screen.grid().at(4).starts_with("       260  root atoms"));
    CHECK(screen.grid().at(7).starts_with(" q quit"));
    // Stopped at q, leaving the rest unread.
    CHECK(screen.keys_left());
}

TEST_CASE("escape and the end of input quit; a resize redraws") {
    FakeScreen escaping{8, 40, {{.kind = KeyKind::resize}, {.kind = KeyKind::escape}}};
    egraph::tui::run(escaping, summary, egraph::glyphs(egraph::GlyphSet::ascii));
    CHECK(escaping.renders == 2);
    CHECK_FALSE(escaping.keys_left());

    FakeScreen ending{8, 40, {}};
    egraph::tui::run(ending, summary, egraph::glyphs(egraph::GlyphSet::ascii));
    CHECK(ending.renders == 1);
}

TEST_CASE("a terminal too small for the layout gets what fits") {
    for (const unsigned rows : {0U, 1U, 2U, 3U}) {
        FakeScreen screen{rows, 5, {}};
        egraph::tui::run(screen, summary, egraph::glyphs(egraph::GlyphSet::ascii));
        CHECK(screen.renders == 1);
    }
}
