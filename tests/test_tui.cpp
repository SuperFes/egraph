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
    Key read(std::optional<std::chrono::milliseconds> timeout) {
        timeouts.push_back(timeout);
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
    // Each read's timeout.
    std::vector<std::optional<std::chrono::milliseconds>> timeouts;

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

// A check whose fresh build is the cyclic store, one package differing.
const egraph::tui::Checker cyclic_check = [](const egraph::Store&) -> egraph::tui::CheckResult {
    return egraph::tui::Fresh{.store = cyclic(), .drift = {"~dev-libs/b-1"}};
};

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
    egraph::tui::run(screen, app, ascii, {.check = no_check});
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
    egraph::tui::run(screen, app, ascii, {.check = no_check});
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
        egraph::tui::run(screen, app, ascii, {.check = no_check});
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
    egraph::tui::run(screen, app, ascii, {.check = no_check});
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
        return egraph::tui::Fresh{.store = sample(), .drift = {"+x/new-1", "~dev-libs/b-1"}};
    };
    egraph::tui::run(screen, app, ascii, {.check = check});
    CHECK(checks == 1);
    CHECK(contains(waiting, "Building a fresh store"));
    REQUIRE(app.checked().has_value());
    CHECK(app.checked()->stage == egraph::tui::Checked::Stage::checked);
    const auto text = screen.text();
    CHECK(contains(text, "differs from a fresh build  2"));
    CHECK(contains(text, " > + x/new-1"));
    CHECK(contains(text, "installed since the store was built"));
    CHECK(contains(text, "~ dev-libs/b-1"));

    // What only the fresh build has cannot be opened, and says so; the rest opens its page, and
    // esc comes back here, then to the list.
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "x/new-1 is not in the store");
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "u shows the fresh build, which has it."));
    // The key that closes a dialog does nothing else.
    app.handle(key(KeyKind::down));
    CHECK_FALSE(app.dialog().has_value());
    CHECK(app.checked()->cursor.at == 0);
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 1);
    app.handle(key(KeyKind::escape));
    CHECK(app.checked().has_value());
    app.handle(character(U'r'));
    CHECK(app.check_requested());
    app.finish_check(egraph::tui::Fresh{.store = sample(), .drift = {}});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "+ The store matches a fresh build"));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.checked().has_value());
}

TEST_CASE("a check that cannot run shows why") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 100, {}};
    app.handle(character(U'c'));
    app.finish_check(
        std::unexpected("egraph-build exited with status 1:\nTraceback\nKeyError: 'x'"));
    egraph::tui::draw(screen, app, ascii);
    // Back at the list, under the dialog.
    CHECK_FALSE(app.checked().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->lines.size() == 3);
    const auto text = screen.text();
    CHECK(contains(text, "+- ! The check could not run -"));
    CHECK(contains(text, "|  egraph-build exited with status 1:"));
    CHECK(contains(text, "|  KeyError: 'x'"));
    CHECK(contains(text, "any key -+"));
    CHECK(contains(screen.line(0), "2 of 2 packages"));
    app.handle(character(U'q'));
    CHECK_FALSE(app.done());
    CHECK_FALSE(app.dialog().has_value());
}

TEST_CASE("a dialog sits centred in its frame and cuts what does not fit") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.show({.error = false, .title = "Warning", .lines = {"short", std::string(300, 'x')}});
    FakeScreen screen{12, 40, {}};
    egraph::tui::draw(screen, app, ascii);
    // Six rows: the edges, a blank margin above and below, and two lines.
    CHECK(screen.line(3) == " +- Warning --------------------------+ ");
    CHECK(screen.line(4) == " |                                    | ");
    CHECK(screen.line(5) == " |  short                             | ");
    CHECK(screen.line(6) == " |  " + std::string(32, 'x') + "  | ");
    CHECK(screen.line(7) == " |                                    | ");
    CHECK(screen.line(8) == " +-------------------------- any key -+ ");

    // More lines than rows: the first ones show, and the frame stays whole.
    app.show({.error = true, .title = "Many", .lines = std::vector<std::string>(20, "line")});
    FakeScreen small{8, 30, {}};
    egraph::tui::draw(small, app, ascii);
    CHECK(contains(small.line(0), "+- ! Many "));
    CHECK(contains(small.line(2), "|  line"));
    CHECK(contains(small.line(5), "|  line"));
    CHECK_FALSE(contains(small.line(6), "line"));
    CHECK(contains(small.line(7), "any key -+"));
}

TEST_CASE("a build-time dependency since replaced is history, not breakage") {
    for (const bool build_time : {true, false}) {
        auto decoded = egraph::decode(egraph::test::newer_wanted(build_time));
        REQUIRE(decoded.has_value());
        const auto& store = *decoded;
        const auto graph = egraph::build_graph(store);
        egraph::tui::App app{store, graph};
        FakeScreen screen{16, 120, {}};
        egraph::tui::draw(screen, app, ascii);
        app.handle(key(KeyKind::enter));
        egraph::tui::draw(screen, app, ascii);
        const auto text = screen.text();
        if (build_time) {
            CHECK(app.broken(0) == 0);
            CHECK(contains(text, "Built with, since replaced  1"));
            CHECK(contains(text, "....B   || ( >=dev-libs/b-2 dev-libs/missing )  > dev-libs/b-1"));
            CHECK_FALSE(contains(text, "Not installed"));
        } else {
            // At run time the package may still need what it asked for.
            CHECK(app.broken(0) == 1);
            CHECK(contains(text, "Not installed  1"));
            CHECK(contains(text, "R.... ! || ( >=dev-libs/b-2 dev-libs/missing )  > dev-libs/b-1"));
        }
    }
}

TEST_CASE("u shows a check's fresh build without saving it") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    CHECK(app.dependents(0) == 0);
    FakeScreen screen{12, 120, {character(U'c')}};
    int rebuilds = 0;
    const egraph::tui::Rebuilder rebuild = [&]() -> std::expected<egraph::Store, std::string> {
        ++rebuilds;
        return cyclic();
    };
    egraph::tui::run(screen, app, ascii, {.check = cyclic_check, .rebuild = rebuild});
    CHECK(contains(screen.text(), "u shows the fresh build, without saving it"));
    CHECK(contains(screen.line(11), "u preview"));
    app.handle(key(KeyKind::enter));
    app.handle(key(KeyKind::escape));
    app.handle(character(U'u'));
    CHECK(rebuilds == 0);
    CHECK(app.source() == egraph::tui::Source::preview);
    CHECK(app.pages().empty());
    // In the fresh build b-1 depends on a-1.
    CHECK(app.dependents(0) == 1);
    CHECK(&app.store() != &store);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(),
                   "+ Showing the fresh build   not saved: only root writes the store"));
    CHECK(screen.line(0).ends_with(" preview, not saved "));
    CHECK_FALSE(contains(screen.line(11), "u preview"));

    // Checking again compares the build on screen.
    app.handle(character(U'r'));
    REQUIRE(app.check_requested());
    const auto* shown = &app.store();
    app.finish_check(egraph::tui::Fresh{.store = cyclic(), .drift = {}});
    CHECK(shown == &app.store());
    app.handle(key(KeyKind::escape));
    egraph::tui::draw(screen, app, ascii);
    CHECK(screen.line(0).ends_with(" preview, not saved "));
    CHECK(contains(screen.text(), "app-misc/a-1"));

    // The adopted store moves with the app.
    auto moved = std::move(app);
    CHECK(moved.dependents(0) == 1);
    moved.handle(key(KeyKind::enter));
    CHECK(moved.pages().size() == 1);
}

TEST_CASE("u rebuilds the store where it can be written") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph, egraph::tui::Update::save};
    FakeScreen screen{12, 120, {character(U'c')}};
    egraph::tui::run(screen, app, ascii, {.check = cyclic_check});
    REQUIRE(app.checked().has_value());
    // Only a preview needs the check's build.
    CHECK_FALSE(app.checked()->fresh.has_value());
    CHECK(contains(screen.text(), "differs from a fresh build  1   u rebuilds it"));
    CHECK(contains(screen.line(11), "u rebuild"));

    app.handle(character(U'u'));
    CHECK(app.rebuild_requested());
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Rebuilding the store"));
    app.handle(character(U'u'));
    app.finish_rebuild(cyclic());
    CHECK(app.source() == egraph::tui::Source::saved);
    CHECK(app.dependents(0) == 1);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "+ Rebuilt the store   showing it now"));
    CHECK_FALSE(contains(screen.line(0), "preview"));
    app.handle(character(U'u'));
    CHECK_FALSE(app.rebuild_requested());
}

TEST_CASE("a failed rebuild leaves the store as it was") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph, egraph::tui::Update::save};
    FakeScreen screen{12, 120, {character(U'c'), character(U'u')}};
    const egraph::tui::Rebuilder rebuild = []() -> std::expected<egraph::Store, std::string> {
        return std::unexpected("egraph-build exited with status 1:\nPermissionError");
    };
    egraph::tui::run(screen, app, ascii, {.check = cyclic_check, .rebuild = rebuild});
    CHECK(app.source() == egraph::tui::Source::opened);
    CHECK(&app.store() == &store);
    CHECK(app.dependents(0) == 0);
    CHECK(contains(screen.text(), "! The rebuild failed; the store is as it was"));
    CHECK(contains(screen.text(), "|  PermissionError"));
    // The drift still shows under the dialog, and u can try again.
    CHECK(contains(screen.text(), "differs from a fresh build  1"));
    app.handle(key(KeyKind::escape));
    CHECK(app.checked().has_value());
    app.handle(character(U'u'));
    CHECK(app.rebuild_requested());
}

TEST_CASE("u does nothing without drift to fix") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    for (const bool fails : {false, true}) {
        egraph::tui::App app{store, graph, egraph::tui::Update::save};
        FakeScreen screen{12, 120, {character(U'u'), character(U'c'), character(U'u')}};
        int rebuilds = 0;
        const egraph::tui::Checker check = [&](const egraph::Store&) -> egraph::tui::CheckResult {
            if (fails) {
                return std::unexpected("no builder");
            }
            return egraph::tui::Fresh{.store = sample(), .drift = {}};
        };
        const egraph::tui::Rebuilder rebuild = [&]() -> std::expected<egraph::Store, std::string> {
            ++rebuilds;
            return sample();
        };
        egraph::tui::run(screen, app, ascii, {.check = check, .rebuild = rebuild});
        CHECK(rebuilds == 0);
        CHECK(app.source() == egraph::tui::Source::opened);
        // A failed check closes its view; the dialog took the second u.
        CHECK(app.checked().has_value() == !fails);
        CHECK_FALSE(app.dialog().has_value());
        if (!fails) {
            CHECK(app.checked()->stage == egraph::tui::Checked::Stage::checked);
            CHECK_FALSE(contains(screen.line(11), "u "));
        }
    }
}

namespace {

// dev-libs/b-2 compiling with cgroup counters, x/new-1 built and waiting to merge, and a
// binary package that has not started a phase.
std::vector<egraph::emerge::Snapshot> running_emerges() {
    using egraph::emerge::TaskKind;
    return {{.pid = 4321,
             .timestamp = 0,
             .jobs = {.running = 3, .max = 4, .completed = 13, .total = 40, .failed = 1},
             .tasks = {{.cpv = "dev-libs/b-2",
                        .kind = TaskKind::build,
                        .phase = "compile",
                        .pid = 5000,
                        .elapsed = 100.5,
                        .build_elapsed = 100.5,
                        .resources = {.cpu_usec = 402000000, .mem_peak = 2097152}},
                       {.cpv = "x/new-1",
                        .kind = TaskKind::merge,
                        .phase = "merge-wait",
                        .merge_wait = true,
                        .elapsed = 80,
                        .build_elapsed = 55.25},
                       {.cpv = "app-misc/baz-1", .binary = true}}}};
}

} // namespace

TEST_CASE("durations, sizes, bars and spinners read at a glance") {
    CHECK(egraph::tui::duration(0.4) == "0s");
    CHECK(egraph::tui::duration(45) == "45s");
    CHECK(egraph::tui::duration(100.5) == "1m40s");
    CHECK(egraph::tui::duration(3725) == "1h02m");
    CHECK(egraph::tui::duration(-3) == "0s");
    CHECK(egraph::tui::byte_size(512) == "512 B");
    CHECK(egraph::tui::byte_size(1536) == "1.5 KiB");
    CHECK(egraph::tui::byte_size(2097152) == "2.0 MiB");
    CHECK(egraph::tui::byte_size(5ULL << 30U) == "5.0 GiB");

    const auto& unicode = egraph::glyphs(egraph::GlyphSet::unicode);
    CHECK(egraph::tui::progress_bar(12, 40, 20, unicode) ==
          "██████" + std::string{} + "░░░░░░░░░░░░░░");
    // 6.5 cells: the half is an eighths glyph, where ascii rounds down.
    CHECK(egraph::tui::progress_bar(13, 40, 20, unicode) == "██████▌░░░░░░░░░░░░░");
    CHECK(egraph::tui::progress_bar(13, 40, 20, ascii) == "######--------------");
    CHECK(egraph::tui::progress_bar(0, 0, 4, ascii) == "----");
    CHECK(egraph::tui::progress_bar(9, 4, 4, ascii) == "####");

    CHECK(egraph::tui::spinner_frame(0, unicode) == "⠋");
    CHECK(egraph::tui::spinner_frame(11, unicode) == "⠙");
    CHECK(egraph::tui::spinner_frame(2, ascii) == "-");
}

TEST_CASE("e watches the running emerges, reading them again each second") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 140, {character(U'e'), key(KeyKind::tick), key(KeyKind::tick)}};
    int reads = 0;
    const egraph::tui::Watcher watch = [&] {
        ++reads;
        return running_emerges();
    };
    egraph::tui::run(screen, app, ascii, {.watch = watch});
    CHECK(reads == 3);
    REQUIRE(app.watched().has_value());
    CHECK(app.watched()->frame == 3);
    // The list waits for keys; the emerge view wakes to read again.
    CHECK(screen.timeouts.front() == std::nullopt);
    CHECK(screen.timeouts.back() == egraph::tui::watch_interval);

    const auto text = screen.text();
    CHECK(contains(screen.line(0), "egraph  > emerge"));
    CHECK(contains(text, " * emerge 4321   3 of 4 jobs   13 of 40 done   ######--------------  32%"
                         "   ! 1 failed"));
    // The spinner turned once a read, and the kinds: built, waiting, binary.
    CHECK(contains(text, " >   |- \\ b dev-libs/b-2"));
    CHECK(contains(text, "compile             1m40s   4.0x CPU   2.0 MiB peak"));
    CHECK(contains(text, "    |-   w x/new-1"));
    CHECK(contains(text, "waiting to merge      55s"));
    CHECK(contains(text, "    `- \\ p app-misc/baz-1"));
    CHECK(contains(text, "starting"));
    CHECK(contains(screen.line(11), "move"));
}

TEST_CASE("the emerge view says how to publish when nothing runs") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 100, {character(U'e')}};
    egraph::tui::run(screen, app, ascii, {});
    CHECK(contains(screen.text(), "No emerge is publishing its progress"));
    CHECK(contains(screen.text(), "FEATURES=\"observability\""));
    // Moving or opening in an empty view does nothing.
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    CHECK_FALSE(app.dialog().has_value());
}

TEST_CASE("a task opens the page of its installed version, and esc comes back") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    REQUIRE(app.watch_requested());
    app.finish_watch(running_emerges());
    CHECK_FALSE(app.watch_requested());
    CHECK(app.refresh() == egraph::tui::watch_interval);

    // dev-libs/b-2 is building; b-1 is what is installed.
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 1);
    // No reads while the page covers the view, and none are owed.
    CHECK(app.refresh() == std::nullopt);
    app.handle(key(KeyKind::tick));
    CHECK_FALSE(app.watch_requested());
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.watch_requested());
    app.finish_watch(running_emerges());

    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "No version of x/new is installed yet");
    // A tick does not close the dialog.
    app.handle(key(KeyKind::tick));
    CHECK(app.dialog().has_value());
    app.handle(key(KeyKind::escape));

    // Fewer tasks on the next read keep the cursor on one of them.
    app.handle(key(KeyKind::end));
    CHECK(app.watched()->cursor.at == 2);
    auto fewer = running_emerges();
    fewer.front().tasks.pop_back();
    app.handle(key(KeyKind::tick));
    app.finish_watch(fewer);
    CHECK(app.watched()->cursor.at == 1);

    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.watched().has_value());
    CHECK(app.refresh() == std::nullopt);
}
