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

// The sample, but b-1 also has RDEPEND app-misc/a, closing a cycle.
egraph::Store cyclic() {
    egraph::test::Bytes packages;
    packages.varint(2);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({6}).list({6}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(1);
    packages.varint(0).varint(0).varint(7).list({1});
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(1);
    packages.varint(0).varint(0).varint(2).list({0});
    packages.varint(0).varint(0);
    const auto bytes = egraph::test::with_section(4, packages);
    auto decoded = egraph::decode(bytes);
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

// The sample, but a-1 only needs b-1 to build: DEPEND dev-libs/b.
egraph::Store build_only() {
    egraph::test::Bytes packages;
    packages.varint(2);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({6}).list({6}).varint(0);
    packages.varint(0).varint(1);
    packages.varint(0).varint(0).varint(7).list({1});
    packages.varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    auto decoded = egraph::decode(egraph::test::with_section(4, packages));
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

const auto& ascii = egraph::glyphs(egraph::GlyphSet::ascii);

const egraph::tui::Checker no_check = [](const egraph::Store&) -> egraph::tui::CheckResult {
    return std::unexpected("no builder in tests");
};

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
    FakeScreen screen{10, 100, {key(KeyKind::down), character(U'q'), character(U'z')}};
    egraph::tui::run(screen, app, ascii, no_check);
    CHECK(contains(screen.line(0), "2 of 2 packages"));
    // a-1 is marked as kept by @selected.
    CHECK(screen.line(3).starts_with("   @  app-misc/a-1"));
    // The cursor moved down to b-1 before q.
    CHECK(app.list().cursor.at == 1);
    CHECK(screen.line(4).starts_with(" >    dev-libs/b-1"));
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
    FakeScreen screen{16, 80, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii, no_check);
    REQUIRE(app.pages().size() == 1);
    const auto& page = app.pages().back();
    CHECK(page.package == 0);
    CHECK(page.rows.at(page.cursor.at).type == RowType::link);
    const auto text = screen.text();
    CHECK(contains(text, "Depends on  1"));
    CHECK(contains(text, "R....   dev-libs/b-1"));
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
        egraph::tui::run(screen, app, ascii, no_check);
        CHECK(screen.renders == 1);
        CHECK(app.done());
    }
}

TEST_CASE("thread draws each level's line down to its last child") {
    // a, with children b (with child c) and d.
    std::vector<egraph::tui::Row> rows(5);
    rows.at(0).type = RowType::heading;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        rows.at(i).type = RowType::link;
    }
    rows.at(1).depth = 0;
    rows.at(2).depth = 1;
    rows.at(3).depth = 2;
    rows.at(4).depth = 1;
    egraph::tui::thread(rows);
    CHECK_FALSE(rows.at(2).last);
    CHECK(rows.at(3).last);
    CHECK(rows.at(3).rails == std::vector<bool>{true});
    CHECK(rows.at(4).last);
    CHECK(rows.at(4).rails.empty());

    // With d gone, nothing follows b at level 1.
    rows.pop_back();
    egraph::tui::thread(rows);
    CHECK(rows.at(2).last);
    CHECK(rows.at(3).rails == std::vector<bool>{false});
}

TEST_CASE("links unfold in place, and stop at a cycle") {
    const auto store = cyclic();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{14, 80, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii, no_check);
    REQUIRE(app.pages().size() == 1);
    CHECK(contains(screen.text(), "R.... + dev-libs/b-1"));
    CHECK(contains(screen.text(), "space unfold"));

    const auto at = app.pages().back().cursor.at;
    app.handle(character(U' '));
    egraph::tui::draw(screen, app, ascii);
    const auto& rows = app.pages().back().rows;
    REQUIRE(rows.at(at + 1).type == RowType::link);
    CHECK(rows.at(at + 1).depth == 1);
    CHECK(rows.at(at + 1).cycle);
    CHECK(contains(screen.text(), "R.... - dev-libs/b-1"));
    CHECK(contains(screen.text(), "R.... ^ `- app-misc/a-1"));

    // The cycle does not unfold; h climbs to its parent, folds it, then goes back.
    app.handle(character(U'j'));
    CHECK(app.pages().back().cursor.at == at + 1);
    app.handle(character(U' '));
    CHECK(app.pages().back().rows.size() == rows.size());
    app.handle(character(U'h'));
    CHECK(app.pages().back().cursor.at == at);
    app.handle(character(U'h'));
    CHECK_FALSE(app.pages().back().rows.at(at).unfolded);
    CHECK(app.pages().back().rows.at(at + 1).type != RowType::link);
    app.handle(character(U'h'));
    CHECK(app.pages().empty());
}

TEST_CASE("l unfolds and then steps in; rdeps unfold upwards") {
    const auto store = cyclic();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.set_height(10);
    app.handle(key(KeyKind::enter));
    const auto at = app.pages().back().cursor.at;
    app.handle(key(KeyKind::right));
    CHECK(app.pages().back().rows.at(at).unfolded);
    app.handle(key(KeyKind::right));
    CHECK(app.pages().back().cursor.at == at + 1);

    // Down past the cycle row to b-1 under Needed by, whose dependents are a-1 again.
    app.handle(key(KeyKind::down));
    const auto& page = app.pages().back();
    REQUIRE(page.rows.at(page.cursor.at).reverse);
    app.handle(key(KeyKind::tab));
    const auto& child = app.pages().back().rows.back();
    CHECK(child.reverse);
    CHECK(child.depth == 1);
    CHECK(child.link.package == 0);
    CHECK(child.cycle);
    app.handle(key(KeyKind::tab));
    CHECK(app.pages().back().rows.back().depth == 0);
}

TEST_CASE("a page starts with the chain that keeps its package") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{16, 80, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(screen.line(3), "Kept by"));
    CHECK(contains(screen.line(4), "@ @selected  app-misc/a"));
    CHECK(contains(screen.line(5), ".....   `- app-misc/a-1"));
    CHECK(contains(screen.line(6), "R....     `- dev-libs/b-1"));
    CHECK(contains(screen.line(6), "dev-libs/b |"));

    // The cursor starts on the dependents; up reaches the chain, and enter follows it, except
    // onto the page's own package.
    const auto& page = app.pages().back();
    CHECK(page.rows.at(page.cursor.at).type == RowType::link);
    app.handle(key(KeyKind::up));
    CHECK(app.pages().back().rows.at(app.pages().back().cursor.at).type == RowType::path);
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().size() == 1);
    app.handle(key(KeyKind::up));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(app.pages().back().package == 0);
}

TEST_CASE("o shows only orphans, and b drops build-time dependencies") {
    const auto store = build_only();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {}};
    app.handle(character(U'o'));
    CHECK(app.list().only == egraph::tui::Only::orphans);
    CHECK(app.list().shown.empty());
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "0 orphans"));
    CHECK(contains(screen.text(), "nothing to remove"));
    CHECK(contains(screen.text(), "o all"));

    app.handle(character(U'b'));
    CHECK_FALSE(app.build_deps());
    CHECK(app.list().shown == std::vector<std::uint32_t>{1});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 orphans  run-time deps only"));
    CHECK(screen.line(3).starts_with(" > -  dev-libs/b-1"));
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Kept by, at run time"));
    CHECK(contains(screen.text(), "- nothing: depclean would remove it"));
}

TEST_CASE("without roots depclean refuses, and the orphans view says so") {
    const auto store = [] {
        auto decoded =
            egraph::decode(egraph::test::with_section(5, egraph::test::Bytes{}.varint(0)));
        REQUIRE(decoded.has_value());
        return std::move(*decoded);
    }();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {}};
    app.handle(character(U'o'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "2 orphans  ! depclean would refuse to run"));
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "nothing: @world is empty"));
}

TEST_CASE("! shows only broken packages, and a page lists what is not installed") {
    // a-1 alone, RDEPEND: dev-libs/missing dev-libs/b, neither installed.
    const auto store = [] {
        auto decoded = egraph::decode(egraph::test::with_rdepend(2, [](egraph::test::Bytes& b) {
            b.varints({0, 0, 14}).list({});
            b.varints({0, 0, 7}).list({});
        }));
        REQUIRE(decoded.has_value());
        return std::move(*decoded);
    }();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    CHECK(app.broken(0) == 2);
    FakeScreen screen{14, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(screen.line(3).starts_with(" > @! app-misc/a-1"));
    CHECK(contains(screen.text(), "! broken"));

    app.handle(character(U'!'));
    CHECK(app.list().shown == std::vector<std::uint32_t>{0});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 broken"));
    CHECK(contains(screen.text(), "! all"));

    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(text, "Not installed  2"));
    CHECK(contains(text, "R.... ! dev-libs/b"));
    CHECK(contains(text, "R.... ! dev-libs/missing"));
}

TEST_CASE("without build-time dependencies, only run-time ones count as broken") {
    // a-1 alone, DEPEND: dev-libs/missing.
    egraph::test::Bytes packages;
    packages.varint(1).varints({1, 2, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(1).varints({0, 0, 14}).list({});
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    auto decoded = egraph::decode(egraph::test::with_section(4, packages));
    REQUIRE(decoded.has_value());
    const auto& store = *decoded;
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'!'));
    CHECK(app.list().shown.size() == 1);
    app.handle(character(U'b'));
    CHECK(app.broken(0) == 0);
    CHECK(app.list().shown.empty());
    app.handle(character(U'!'));
    app.handle(key(KeyKind::enter));
    const auto& rows = app.pages().back().rows;
    CHECK(std::ranges::none_of(rows, [](const auto& row) { return row.type == RowType::missing; }));
}

TEST_CASE("c checks the store against a fresh build, showing a wait first") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {character(U'c')}};
    std::string waiting;
    int checks = 0;
    const egraph::tui::Checker check =
        [&](const egraph::Store& stored) -> egraph::tui::CheckResult {
        CHECK(&stored == &store);
        waiting = screen.text();
        ++checks;
        return std::vector<std::string>{"+x/new-1", "~dev-libs/b-1"};
    };
    egraph::tui::run(screen, app, ascii, check);
    CHECK(checks == 1);
    CHECK(contains(waiting, "Building a fresh store"));
    REQUIRE(app.checked().has_value());
    CHECK_FALSE(app.checked()->running);
    const auto text = screen.text();
    CHECK(contains(text, "differs from a fresh build  2"));
    CHECK(contains(text, " > + x/new-1"));
    CHECK(contains(text, "installed since the store was built"));
    CHECK(contains(text, "~ dev-libs/b-1"));

    // What only the fresh build has cannot be opened; the rest opens its page, and esc comes
    // back here, then to the list.
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 1);
    app.handle(key(KeyKind::escape));
    CHECK(app.checked().has_value());
    app.handle(character(U'r'));
    CHECK(app.check_requested());
    app.finish_check(std::vector<std::string>{});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "+ The store matches a fresh build"));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.checked().has_value());
}

TEST_CASE("a check that cannot run shows why") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 100, {character(U'c'), character(U'j'), character(U'q')}};
    const egraph::tui::Checker check = [](const egraph::Store&) -> egraph::tui::CheckResult {
        return std::unexpected("egraph-build exited with status 1:\nTraceback\nKeyError: 'x'");
    };
    egraph::tui::run(screen, app, ascii, check);
    CHECK(app.done());
    const auto text = screen.text();
    CHECK(contains(text, "! The check could not run"));
    CHECK(contains(screen.line(3), "egraph-build exited with status 1:"));
    CHECK(contains(screen.line(5), "KeyError: 'x'"));
    CHECK(screen.line(11).starts_with(" r again"));
}
