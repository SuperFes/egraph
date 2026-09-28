#include "tui.hpp"

#include "store_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

using egraph::tui::Key;
using egraph::tui::KeyKind;
using egraph::tui::RowType;

namespace {

// A grid of cells (one code point each) in place of a terminal, fed keys from a script.
class FakeScreen {
  public:
    FakeScreen(unsigned rows, unsigned cols, std::deque<Key> keys)
        : rows_(rows), cols_(cols), keys_(std::move(keys)) {
        clear();
    }

    [[nodiscard]] egraph::tui::Size size() const { return {.rows = rows_, .cols = cols_}; }
    void clear() { cells_.assign(rows_, std::vector<std::string>(cols_, " ")); }
    void put(unsigned row, unsigned col, std::string_view text, const egraph::tui::Pen&) {
        for (std::size_t i = 0; i < text.size() && row < rows_ && col < cols_; ++col) {
            std::size_t length = 1;
            while (i + length < text.size() &&
                   (static_cast<unsigned char>(text.at(i + length)) & 0xC0U) == 0x80U) {
                ++length;
            }
            cells_.at(row).at(col) = std::string{text.substr(i, length)};
            i += length;
        }
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

    [[nodiscard]] std::string line(unsigned row) const {
        std::string out;
        for (const auto& cell : cells_.at(row)) {
            out += cell;
        }
        return out;
    }
    [[nodiscard]] std::string text() const {
        std::string out;
        for (unsigned row = 0; row < rows_; ++row) {
            out += line(row) + '\n';
        }
        return out;
    }
    [[nodiscard]] bool keys_left() const { return !keys_.empty(); }
    int renders = 0;

  private:
    unsigned rows_;
    unsigned cols_;
    std::vector<std::vector<std::string>> cells_;
    std::deque<Key> keys_;
};

Key character(char32_t code) {
    return {.kind = KeyKind::character, .code = code};
}

Key key(KeyKind kind) {
    return {.kind = kind};
}

// app-misc/a-1 RDEPEND "|| ( dev-libs/b dev-libs/missing ) !app-misc/old", and dev-libs/b-1.
egraph::Store sample() {
    auto store = egraph::decode(egraph::test::fresh_sample());
    REQUIRE(store.has_value());
    return std::move(*store);
}

const auto& ascii = egraph::glyphs(egraph::GlyphSet::ascii);

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("links group a package's edges by package and atom") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    const auto deps = egraph::tui::links(store, graph, 0, false);
    REQUIRE(deps.size() == 1);
    CHECK(deps.front().package == 1);
    CHECK(deps.front().choice);
    // RDEPEND, the first shorthand.
    CHECK(deps.front().kinds == std::array<bool, 5>{true, false, false, false, false});
    const auto rdeps = egraph::tui::links(store, graph, 1, true);
    REQUIRE(rdeps.size() == 1);
    CHECK(rdeps.front().package == 0);
}

TEST_CASE("the list shows every package and quits on q") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{10, 60, {key(KeyKind::down), character(U'q'), character(U'z')}};
    egraph::tui::run(screen, app, ascii);
    CHECK(contains(screen.line(0), "2 of 2 packages"));
    CHECK(contains(screen.line(3), "app-misc/a-1"));
    CHECK(contains(screen.line(4), "dev-libs/b-1"));
    // The cursor moved down to b-1 before q.
    CHECK(app.list().cursor.at == 1);
    CHECK(screen.line(4).starts_with(" > dev-libs/b-1"));
    CHECK(contains(screen.line(9), "q quit"));
    CHECK(screen.keys_left());
}

TEST_CASE("typing after / filters the list as it goes") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    for (const auto& pressed :
         {character(U'/'), character(U'L'), character(U'I'), character(U'B')}) {
        app.handle(pressed);
    }
    CHECK(app.list().searching);
    CHECK(app.list().shown == std::vector<std::uint32_t>{1});
    app.handle(key(KeyKind::backspace));
    CHECK(app.list().query == "LI");
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.list().searching);
    // q is a key again once the search is kept.
    app.handle(key(KeyKind::escape));
    CHECK(app.list().query.empty());
    CHECK(app.list().shown.size() == 2);
    app.handle(character(U'/'));
    app.handle(character(U'q'));
    CHECK_FALSE(app.done());
    CHECK(app.list().shown.empty());
}

TEST_CASE("enter opens a package's page, and pages open from there") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 80, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii);
    REQUIRE(app.pages().size() == 1);
    const auto& page = app.pages().back();
    CHECK(page.package == 0);
    CHECK(page.rows.at(page.cursor.at).type == RowType::link);
    const auto text = screen.text();
    CHECK(contains(text, "Depends on  1"));
    CHECK(contains(text, "R....  dev-libs/b-1"));
    CHECK(contains(text, "dev-libs/b |"));
    CHECK(contains(text, "Needed by  0"));
    CHECK(contains(text, "nothing"));
    CHECK(contains(screen.line(0), "> a"));

    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(app.pages().back().package == 1);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "> a > b"));
    CHECK(contains(screen.text(), "Needed by  1"));

    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::backspace));
    CHECK(app.pages().empty());
    app.handle(character(U'q'));
    CHECK(app.done());
}

TEST_CASE("the end of input quits, and small terminals get nothing drawn") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    for (const unsigned rows : {0U, 1U, 4U, 5U}) {
        egraph::tui::App app{store, graph};
        FakeScreen screen{rows, 12, {}};
        egraph::tui::run(screen, app, ascii);
        CHECK(screen.renders == 1);
        CHECK(app.done());
    }
}
