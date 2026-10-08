#include "tui.hpp"

#include "helpers.hpp"
#include "index_builder.hpp"
#include "store_writer.hpp"
#include "system_builder.hpp"
#include "use_ledger_builder.hpp"

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
    void suspend() {
        suspended = true;
        ++suspends;
    }
    std::expected<void, std::string> resume() {
        suspended = false;
        if (!resumable) {
            return std::unexpected(std::string{"cannot start the terminal interface"});
        }
        return {};
    }
    [[nodiscard]] bool keys_left() const { return !keys_.empty(); }
    int renders = 0;
    bool suspended = false;
    int suspends = 0;
    bool resumable = true;
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
    packages.varints({1, 2, 3, 3, 4, 5, 1, 0, 0}).list({6}).list({6}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(1);
    packages.varint(0).varint(0).varint(7).list({1});
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1, 0, 0}).list({}).list({}).varint(0);
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
    packages.varints({1, 2, 3, 3, 4, 5, 1, 0, 0}).list({6}).list({6}).varint(0);
    packages.varint(0).varint(1);
    packages.varint(0).varint(0).varint(7).list({1});
    packages.varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1, 0, 0}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    auto decoded = egraph::decode(egraph::test::with_section(4, packages));
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

// The evaluated store beside the sample (store_writer.hpp): a-1 depends on dev-libs/b:= under
// dynamic deps, would depend on dev-libs/b with +flag and on dev-libs/gone with +flag -minimal,
// and is rebuilt for flag* -new%; b-1 is masked, not visible, and upgrades to b-2.
egraph::Evaluated decoded_evaluated(const std::vector<std::byte>& bytes) {
    auto evaluated = egraph::decode_evaluated(bytes);
    REQUIRE(evaluated.has_value());
    return std::move(*evaluated);
}

egraph::Evaluated evaluated_sample() {
    return decoded_evaluated(egraph::test::assemble_evaluated(egraph::test::evaluated_sections()));
}

// The evaluated sample with its dependencies section replaced.
egraph::Evaluated evaluated_sample(const egraph::test::Bytes& dependencies) {
    return decoded_evaluated(egraph::test::evaluated_with_section(4, dependencies));
}

egraph::Stores both() {
    return {.installed = sample(), .evaluated = evaluated_sample()};
}

// Both, but nothing pending for a-1.
egraph::Stores only_b_updates() {
    egraph::test::Bytes dependencies;
    dependencies.varint(2);
    dependencies.varints({1, 0, 3}).varint(1).varint(11).varint(12);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(1);
    dependencies.varints({0, 0, 4}).list({1});
    dependencies.varint(2);
    dependencies.varints({4, 13, 0}).list({1}).list({8});
    dependencies.varints({0, 14, 1}).list({}).list({8, 15});
    dependencies.varints({1, 0, 1, 0}).list({}).varints({0, 0, 0, 0, 0, 0, 0, 0});
    dependencies.varints({2, 1, 3}).varint(0);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(0).varint(0);
    dependencies.varints({0, 1, 1, 3}).list({}).varints({0, 0, 0, 0, 0, 0, 0, 0});
    return {.installed = sample(), .evaluated = evaluated_sample(dependencies)};
}

const auto& ascii = egraph::glyphs(egraph::GlyphSet::ascii);

// Services whose jobs have their results at once.
egraph::tui::Checker checking(std::function<egraph::tui::CheckResult(const egraph::Store&)> check) {
    return [check = std::move(check)](const egraph::Store& stored) {
        return egraph::ready(check(stored));
    };
}
egraph::tui::Rebuilder
rebuilding(std::function<std::expected<egraph::Stores, std::string>()> rebuild) {
    return [rebuild = std::move(rebuild)] {
        return egraph::ready(rebuild().transform([](egraph::Stores stores) {
            return std::make_shared<const egraph::Stores>(std::move(stores));
        }));
    };
}

// A check whose fresh build is the cyclic store, one package differing.
const egraph::tui::Checker cyclic_check =
    checking([](const egraph::Store&) -> egraph::tui::CheckResult {
        return egraph::tui::Fresh{.store = cyclic(), .drift = {"~dev-libs/b-1"}};
    });

const egraph::tui::Checker no_check =
    checking([](const egraph::Store&) -> egraph::tui::CheckResult {
        return std::unexpected("no builder in tests");
    });

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
    CHECK(contains(screen.line(9), "? keys"));
    CHECK(screen.keys_left());
}

TEST_CASE("the hint bar shows the keys for what is selected, and ? lists them all") {
    egraph::tui::App app{only_b_updates(), true};
    FakeScreen screen{24, 120, {}};
    const auto bar = [&] {
        egraph::tui::draw(screen, app, ascii);
        return screen.line(23);
    };
    // On b-1's update, the updates shown.
    CHECK(contains(bar(), " enter open  space pick  U update  u all  "));
    CHECK(contains(bar(), "? keys"));
    CHECK_FALSE(contains(bar(), "q quit"));
    CHECK_FALSE(contains(bar(), "o orphans"));
    CHECK_FALSE(contains(bar(), "r remove"));

    app.handle(character(U'?'));
    CHECK(app.keys_shown());
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(text, " Keys "));
    CHECK(contains(text, "j/k    move"));
    CHECK(contains(text, "o      orphans"));
    CHECK(contains(text, "s      search the repositories"));
    CHECK(contains(text, "q      quit"));
    // The next key only closes the list.
    app.handle(character(U'q'));
    CHECK_FALSE(app.keys_shown());
    CHECK_FALSE(app.done());

    // a-1 has no update; picked, it can be removed.
    app.handle(character(U'u'));
    app.handle(key(KeyKind::home));
    CHECK_FALSE(contains(bar(), "U update"));
    CHECK_FALSE(contains(bar(), "u updates"));
    app.handle(character(U' '));
    CHECK(contains(bar(), "U update  r remove"));
    app.handle(key(KeyKind::home));
    app.handle(character(U' '));
    CHECK(app.picked().empty());
    // The pick taken back, on to b-1.
    CHECK(contains(bar(), " enter open  space pick  U update  "));
    CHECK_FALSE(contains(bar(), "r remove"));

    // While a filter is typed, its keys are all there is and ? is part of it.
    app.handle(character(U'/'));
    CHECK(contains(bar(), " enter keep  esc clear  type to filter"));
    CHECK_FALSE(contains(bar(), "? keys"));
    app.handle(character(U'?'));
    CHECK_FALSE(app.keys_shown());
    CHECK(app.list().query == "?");
    app.handle(key(KeyKind::escape));
    CHECK(app.list().query.empty());

    // On a page, back is always there.
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(contains(bar(), "esc back"));
    CHECK_FALSE(contains(bar(), "q quit"));
    app.handle(character(U'?'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "q      quit"));
}

TEST_CASE("the keys overlay aligns each key's meaning") {
    const std::vector<egraph::tui::Hint> hints{{.key = "j/k", .meaning = "move", .bar = false},
                                               {.key = "enter", .meaning = "open", .bar = true},
                                               {.key = "q", .meaning = "quit", .bar = false}};
    const auto dialog = egraph::tui::keys_dialog(hints);
    CHECK_FALSE(dialog.error);
    CHECK_FALSE(dialog.question);
    CHECK(dialog.title == "Keys");
    CHECK(dialog.lines == std::vector<std::string>{"j/k    move", "enter  open", "q      quit"});
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
    CHECK_FALSE(contains(screen.text(), "! broken"));

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
    packages.varint(1).varints({1, 2, 3, 3, 4, 5, 1, 0, 0}).list({}).list({}).varint(0);
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
        checking([&](const egraph::Store& stored) -> egraph::tui::CheckResult {
            CHECK(&stored == &store);
            waiting = screen.text();
            ++checks;
            return egraph::tui::Fresh{.store = sample(), .drift = {"+x/new-1", "~dev-libs/b-1"}};
        });
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

TEST_CASE("a check's build runs in the background while the screen still answers") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {character(U'c'), key(KeyKind::tick), key(KeyKind::tick)}};
    int polls = 0;
    std::vector<std::string> waiting;
    const egraph::tui::Checker check =
        [&](const egraph::Store&) -> egraph::Job<egraph::tui::CheckResult> {
        return [&]() -> std::optional<egraph::tui::CheckResult> {
            waiting.push_back(screen.text());
            if (++polls < 3) {
                return std::nullopt;
            }
            return egraph::tui::Fresh{.store = sample(), .drift = {}};
        };
    };
    egraph::tui::run(screen, app, ascii, {.check = check});
    CHECK(polls == 3);
    // Read with a timeout while it runs, so the spinner turns; then without.
    REQUIRE(screen.timeouts.size() >= 3);
    CHECK(screen.timeouts.at(1) == egraph::tui::wait_interval);
    CHECK(screen.timeouts.at(2) == egraph::tui::wait_interval);
    CHECK(screen.timeouts.back() == std::nullopt);
    REQUIRE(waiting.size() == 3);
    CHECK(contains(waiting.at(0), "| Building a fresh store"));
    CHECK(contains(waiting.at(1), "/ Building a fresh store"));
    CHECK(contains(waiting.at(0), "esc stop"));
    CHECK(contains(screen.text(), "+ The store matches a fresh build"));
}

TEST_CASE("leaving the check view stops its build") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    for (const auto leave : {key(KeyKind::escape), character(U'q')}) {
        egraph::tui::App app{store, graph};
        FakeScreen screen{12, 120, {character(U'c'), leave}};
        const auto alive = std::make_shared<int>(0);
        const egraph::tui::Checker check =
            [&](const egraph::Store&) -> egraph::Job<egraph::tui::CheckResult> {
            return [alive]() -> std::optional<egraph::tui::CheckResult> { return std::nullopt; };
        };
        egraph::tui::run(screen, app, ascii, {.check = check});
        CHECK(alive.use_count() == 1);
        if (leave.kind == KeyKind::escape) {
            CHECK_FALSE(app.checked().has_value());
        }
    }
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
    const egraph::tui::Rebuilder rebuild =
        rebuilding([&]() -> std::expected<egraph::Stores, std::string> {
            ++rebuilds;
            return egraph::Stores{.installed = cyclic(), .evaluated = {}};
        });
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
    app.finish_rebuild(std::make_shared<const egraph::Stores>(
        egraph::Stores{.installed = cyclic(), .evaluated = {}}));
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
    const egraph::tui::Rebuilder rebuild =
        rebuilding([]() -> std::expected<egraph::Stores, std::string> {
            return std::unexpected("egraph-build exited with status 1:\nPermissionError");
        });
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
        const egraph::tui::Checker check =
            checking([&](const egraph::Store&) -> egraph::tui::CheckResult {
                if (fails) {
                    return std::unexpected("no builder");
                }
                return egraph::tui::Fresh{.store = sample(), .drift = {}};
            });
        const egraph::tui::Rebuilder rebuild =
            rebuilding([&]() -> std::expected<egraph::Stores, std::string> {
                ++rebuilds;
                return egraph::Stores{.installed = sample(), .evaluated = {}};
            });
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

namespace {

// A clock a second on at each wait, as the emerge view's own wakes are.
egraph::tui::Clock by_the_second(const FakeScreen& screen) {
    return [&screen] {
        return std::chrono::steady_clock::time_point{} +
               egraph::tui::watch_interval * static_cast<int>(screen.timeouts.size());
    };
}

} // namespace

TEST_CASE("the emerge view reads once a second, however often the loop wakes") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    // Woken every 250 ms, as while a refresh runs.
    std::deque keys{character(U'e')};
    keys.insert(keys.end(), 7, key(KeyKind::tick));
    FakeScreen screen{12, 140, keys};
    int reads = 0;
    int samples = 0;
    egraph::tui::run(screen, app, ascii,
                     {.watch =
                          [&] {
                              ++reads;
                              return running_emerges();
                          },
                      .sample =
                          [&] {
                              ++samples;
                              return egraph::pressure::Sample{.load = 1.0};
                          },
                      .now =
                          [&screen] {
                              return std::chrono::steady_clock::time_point{} +
                                     std::chrono::milliseconds{250} *
                                         static_cast<int>(screen.timeouts.size());
                          }});
    // At once on opening, then a second later; not on the ticks between.
    CHECK(reads == 2);
    CHECK(samples == 2);
    REQUIRE(app.watched().has_value());
    CHECK(app.watched()->history.readings().size() == 2);
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
    egraph::tui::run(screen, app, ascii, {.watch = watch, .now = by_the_second(screen)});
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
    CHECK(contains(screen.line(11), "enter open"));
}

TEST_CASE("the emerge view says how to publish when nothing runs") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 100, {character(U'e')}};
    egraph::tui::run(screen, app, ascii, {});
    CHECK(contains(screen.text(), "No emerge or egraph exec is publishing its progress"));
    CHECK(contains(screen.text(), "FEATURES=\"observability\""));
    CHECK(contains(screen.text(), "egraph exec to /run/egraph"));
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
    // Out of date: read at once, not at the next second.
    CHECK(app.watch_requested());
    CHECK(app.watched()->urgent);
    app.finish_watch(running_emerges());
    CHECK_FALSE(app.watched()->urgent);
    app.handle(key(KeyKind::tick));
    CHECK(app.watch_requested());
    CHECK_FALSE(app.watched()->urgent);
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

namespace {

std::string joined(const std::vector<egraph::tui::Span>& spans) {
    std::string text;
    for (const auto& span : spans) {
        text += span.text;
    }
    return text;
}

// Two seconds of a 4-CPU machine with 8 GiB, half of the CPU time busy between them.
egraph::pressure::History two_samples(bool psi) {
    egraph::pressure::History history;
    const auto stalls = psi ? egraph::pressure::Stalls{.cpu = 12.5, .memory = 0.0, .io = 3.25}
                            : egraph::pressure::Stalls{};
    for (std::uint64_t tick = 1; tick <= 2; ++tick) {
        history.add({.cpu = egraph::pressure::CpuTimes{.busy = tick * 200, .total = tick * 400},
                     .cpus = 4,
                     .mem_total = 8ULL << 30U,
                     .mem_available = (8ULL - tick * 2) << 30U,
                     .load = 2.0 * static_cast<double>(tick),
                     .stalls = stalls});
    }
    return history;
}

} // namespace

TEST_CASE("sparklines scale to their maximum and mark what crosses the limit") {
    const auto& unicode = egraph::glyphs(egraph::GlyphSet::unicode);
    const std::vector<std::optional<double>> values{0.0, 50.0, std::nullopt, 100.0, 200.0};
    const auto spans = egraph::tui::sparkline(values, 100, 7, unicode, egraph::Tone::choice, 60);
    // Two blanks for the time before the first reading, one for the missing one.
    CHECK(joined(spans) == "  ▁▅ ██");
    // The last two cross the limit: one span for them, in the bad tone.
    REQUIRE(spans.size() == 2);
    CHECK(spans.back().text == "██");
    CHECK(spans.back().pen.fg->red == egraph::tui::tone_pen(egraph::Tone::bad).fg->red);
    // Only the last width values show.
    CHECK(joined(egraph::tui::sparkline(values, 100, 2, unicode, egraph::Tone::choice)) == "██");
    CHECK(joined(egraph::tui::sparkline({}, 100, 3, ascii, egraph::Tone::choice)) == "   ");
}

TEST_CASE("the pressure panel shows CPU, memory, load and stalls, with steve's limits") {
    const auto lines = egraph::tui::pressure_lines(
        two_samples(true), {.load = 3.0, .min_available = 4ULL << 30U}, 4, ascii);
    REQUIRE(lines.size() == egraph::tui::pressure_rows);
    CHECK(joined(lines.at(0)) == " System");
    // The first sample has nothing to measure CPU against. Load's second reading is over steve's
    // limit, drawn in the bad tone; memory's is at it, which is not.
    CHECK(joined(lines.at(1)) == " CPU         =     50%   4 CPUs");
    CHECK(joined(lines.at(2)) ==
          " Memory     -=  4.0 GiB available of 8.0 GiB   steve waits under 4.0 GiB");
    CHECK(joined(lines.at(3)) == " Load       =#    4.00   steve waits over 3");
    CHECK(joined(lines.at(4)) == " Stalls   cpu 12.5%  memory 0.0%  io 3.2%   of the last 10 s");

    const auto bare = egraph::tui::pressure_lines(two_samples(false), {}, 4, ascii);
    CHECK(contains(joined(bare.at(4)), "psi=1"));
    CHECK_FALSE(contains(joined(bare.at(3)), "steve"));
    const auto empty = egraph::tui::pressure_lines(egraph::pressure::History{}, {}, 4, ascii);
    CHECK(joined(empty.at(1)) == " CPU                -");
}

TEST_CASE("the emerge view shows the pressure panel where it fits") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    const auto history = two_samples(false);
    app.finish_watch(running_emerges(), *history.latest());
    FakeScreen tall{16, 120, {}};
    egraph::tui::draw(tall, app, ascii);
    CHECK(tall.line(8) == std::string(120, ' '));
    CHECK(tall.line(9).starts_with(" System"));
    CHECK(tall.line(11).starts_with(" Memory"));
    CHECK(tall.line(13).starts_with(" Stalls"));
    CHECK(tall.line(14).starts_with(" Steve    not running"));
    // Room for the emerge and its three tasks above.
    CHECK(contains(tall.text(), "app-misc/baz-1"));
    FakeScreen short_screen{12, 120, {}};
    egraph::tui::draw(short_screen, app, ascii);
    CHECK_FALSE(contains(short_screen.text(), " System"));
}

namespace {

// steve as the dev box runs it, 7 of its 12 jobs handed out.
egraph::steve::Status live_steve() {
    return {.live = true,
            .settings = {.tokens = 5,
                         .jobs = 12,
                         .min_jobs = 1,
                         .load_average = 12,
                         .recheck_timeout = 0.5,
                         .min_memory = 4096}};
}

} // namespace

TEST_CASE("steve's limits mark the graphs") {
    const auto limits = egraph::tui::limits_of(live_steve());
    CHECK(limits.load == 12.0);
    CHECK(limits.min_available == 4096ULL << 20U);
    CHECK_FALSE(egraph::tui::limits_of(std::nullopt).load.has_value());
    auto unlimited = live_steve();
    unlimited.settings.load_average.reset();
    unlimited.settings.min_memory.reset();
    CHECK_FALSE(egraph::tui::limits_of(unlimited).min_available.has_value());
}

TEST_CASE("steve's line shows its jobs and settings, and which one is changing") {
    CHECK(joined(egraph::tui::steve_line(std::nullopt, std::nullopt, ascii)) ==
          " Steve    not running");
    CHECK(joined(egraph::tui::steve_line(live_steve(), std::nullopt, ascii)) ==
          " Steve    #######-----  7 of 12 jobs in use   jobs 12  min jobs 1  load 12  memory 4.0 "
          "GiB  per process -  recheck 0.5s");

    const auto editing = egraph::tui::steve_line(live_steve(), 2, ascii);
    CHECK(contains(joined(editing), "load  12   memory"));
    const auto load = std::ranges::find(editing, std::string{" 12 "}, &egraph::tui::Span::text);
    REQUIRE(load != editing.end());
    CHECK(load->pen.bg.has_value());

    auto read_only = live_steve();
    read_only.live = false;
    read_only.settings.tokens.reset();
    read_only.problem = "unable to open /dev/steve: Permission denied";
    CHECK(joined(egraph::tui::steve_line(read_only, std::nullopt, ascii)) ==
          " Steve    jobs 12  min jobs 1  load 12  memory 4.0 GiB  per process -  recheck 0.5s   "
          "from its command line (unable to open /dev/steve: Permission denied)");
}

TEST_CASE("s changes steve's settings in place, each step at once") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    app.finish_watch({}, {}, live_steve());
    app.handle(character(U's'));
    REQUIRE(app.watched()->editing == 0);
    app.handle(key(KeyKind::left));
    CHECK(app.watched()->editing == 0);
    app.handle(key(KeyKind::right));
    CHECK(app.watched()->editing == 1);
    // min-jobs up by one, asked of run() and not yet made.
    app.handle(character(U'+'));
    auto change = app.steve_change_requested();
    REQUIRE(change.has_value());
    CHECK(change->setting == egraph::steve::Setting::min_jobs);
    CHECK(change->value == 2.0);
    app.finish_steve_change({});
    CHECK_FALSE(app.steve_change_requested().has_value());
    // Read again at once, to show what steve made of it.
    CHECK(app.watch_requested());
    app.finish_watch({}, {}, live_steve());

    // Down arrows change the setting rather than move.
    app.handle(key(KeyKind::right));
    app.handle(key(KeyKind::down));
    change = app.steve_change_requested();
    REQUIRE(change.has_value());
    CHECK(change->setting == egraph::steve::Setting::load_average);
    CHECK(change->value == 11.0);
    app.finish_steve_change(std::unexpected("ioctl failed: Invalid argument"));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "stevie could not change it");
    CHECK(app.dialog()->lines == std::vector<std::string>{"ioctl failed: Invalid argument"});
    app.handle(key(KeyKind::escape));

    // A setting at its floor does not move.
    app.handle(key(KeyKind::right));
    app.handle(key(KeyKind::right));
    app.handle(key(KeyKind::left));
    CHECK(app.watched()->editing == 3);
    auto floor = live_steve();
    floor.settings.min_memory.reset();
    app.finish_watch({}, {}, floor);
    app.handle(character(U'-'));
    CHECK_FALSE(app.steve_change_requested().has_value());

    // Esc and s leave the settings, not the view.
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.watched()->editing.has_value());
    CHECK(app.watched().has_value());
    app.handle(character(U's'));
    app.handle(character(U's'));
    CHECK_FALSE(app.watched()->editing.has_value());
}

TEST_CASE("steve read from its command line, or not running, cannot be changed") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    auto read_only = live_steve();
    read_only.live = false;
    read_only.problem = "unable to open /dev/steve: Permission denied";
    app.finish_watch({}, {}, read_only);
    app.handle(character(U's'));
    CHECK_FALSE(app.watched()->editing.has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "steve's settings can only be read here");
    CHECK(app.dialog()->lines.front() == read_only.problem);
    app.handle(key(KeyKind::escape));

    app.handle(key(KeyKind::tick));
    app.finish_watch({}, {}, std::nullopt);
    app.handle(character(U's'));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "steve is not running");
}

TEST_CASE("run makes steve's changes through stevie and reads the result") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{20, 140, {character(U'e'), character(U's'), character(U'+')}};
    auto steve = live_steve();
    std::vector<std::pair<egraph::steve::Setting, double>> changes;
    const egraph::tui::SteveReader read = [&] { return std::optional{steve}; };
    const egraph::tui::SteveSetter set = [&](egraph::steve::Setting setting,
                                             double value) -> std::expected<void, std::string> {
        changes.emplace_back(setting, value);
        steve.settings.jobs = static_cast<std::int64_t>(value);
        return {};
    };
    egraph::tui::run(screen, app, ascii, {.steve = read, .set_steve = set});
    REQUIRE(changes.size() == 1);
    CHECK(changes.front() == std::pair{egraph::steve::Setting::jobs, 13.0});
    CHECK(contains(screen.text(), " 13 "));
    CHECK(contains(screen.line(19), "+/- change"));
}

namespace {

// lib is building; app waits for it and plugin for app; doc is a binary package waiting for
// nothing. x/other-1 runs but is not on the list.
egraph::emerge::Snapshot building_lib() {
    return {.pid = 4321,
            .timestamp = 0,
            .jobs = {.running = 2, .max = 4, .completed = 1, .total = 5},
            .tasks = {{.cpv = "x/other-1", .phase = "compile"},
                      {.cpv = "dev-libs/lib-1", .phase = "configure"}}};
}

std::vector<egraph::emerge::Pending> lib_merge_list() {
    return {{.kind = "ebuild", .root = "/", .cpv = "dev-libs/lib-1"},
            {.kind = "ebuild", .root = "/", .cpv = "app-misc/app-1"},
            {.kind = "ebuild", .root = "/", .cpv = "app-misc/plugin-1"},
            {.kind = "binary", .root = "/", .cpv = "app-doc/doc-1"}};
}

const egraph::emerge::Waits lib_waits{{"dev-libs/lib-1", {}},
                                      {"app-misc/app-1", {"dev-libs/lib-1"}},
                                      {"app-misc/plugin-1", {"app-misc/app-1"}},
                                      {"app-doc/doc-1", {}}};

} // namespace

TEST_CASE("the merge list hangs under the emerge that runs it, what is running on top") {
    egraph::tui::Watched watched;
    watched.snapshots = {{.pid = 99, .timestamp = 0, .tasks = {{.cpv = "y/elsewhere-1"}}},
                         building_lib()};
    watched.merge_list = lib_merge_list();
    watched.waits = lib_waits;
    CHECK(egraph::tui::rows_owner(watched) == 1);
    const auto rows = egraph::tui::watch_rows(watched);
    std::vector<std::tuple<std::size_t, std::string, std::size_t, bool>> shape;
    for (const auto& row : rows) {
        shape.emplace_back(row.snapshot, row.cpv, row.depth, row.task.has_value());
    }
    CHECK(shape == std::vector<std::tuple<std::size_t, std::string, std::size_t, bool>>{
                       {0, "y/elsewhere-1", 1, true},
                       {1, "x/other-1", 1, true},
                       {1, "dev-libs/lib-1", 1, true},
                       {1, "app-misc/app-1", 2, false},
                       {1, "app-misc/plugin-1", 3, false},
                       {1, "app-doc/doc-1", 1, false}});
    CHECK(rows.at(0).last);
    CHECK_FALSE(rows.at(2).last);
    CHECK(rows.at(3).waiting_for == 1);
    CHECK(rows.at(5).binary);
    CHECK(rows.at(5).last);
    // The top level's line passes app and plugin on its way to doc.
    CHECK(rows.at(4).rails == std::vector<bool>{true, false});

    // Without the list, only what runs.
    watched.merge_list.clear();
    CHECK(egraph::tui::watch_rows(watched).size() == 3);
}

TEST_CASE("egraph exec is named as such, and never takes emerge's merge list") {
    egraph::tui::Watched watched;
    auto run = building_lib();
    run.publisher = egraph::emerge::Publisher::exec;
    watched.snapshots = {run};
    watched.merge_list = lib_merge_list();
    watched.waits = lib_waits;
    CHECK(egraph::tui::rows_owner(watched) == 1);
    CHECK(std::ranges::none_of(egraph::tui::watch_rows(watched),
                               [](const auto& row) { return !row.task.has_value(); }));
    const auto spans = egraph::tui::emerge_spans(run, ascii);
    CHECK(spans.front().text == " * egraph exec 4321");
    // Beside an emerge between tasks, the list is the emerge's.
    watched.snapshots = {{.pid = 99, .timestamp = 0, .tasks = {}}, run};
    CHECK(egraph::tui::rows_owner(watched) == 0);
}
TEST_CASE("the emerge view draws the merge list as a tree that says what can start") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    app.finish_watch({building_lib()}, {}, std::nullopt, lib_merge_list());
    REQUIRE(app.plan_requested().has_value());
    app.finish_plan(lib_waits);
    FakeScreen screen{12, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(text, "  |- / b x/other-1"));
    CHECK(contains(text, "  |- / b dev-libs/lib-1"));
    CHECK(contains(text, "  | `-   o app-misc/app-1"));
    CHECK(contains(text, "waits for 1"));
    CHECK(contains(text, "  |   `-   o app-misc/plugin-1"));
    CHECK(contains(text, "  `-   p app-doc/doc-1"));
    CHECK(contains(text, "ready"));
}

TEST_CASE("the merge list is planned once, and a failure is not asked again") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    app.finish_watch({building_lib()}, {}, std::nullopt, lib_merge_list());
    const auto asked = app.plan_requested();
    REQUIRE(asked.has_value());
    CHECK(asked->size() == 4);
    app.finish_plan(std::unexpected("no ebuild for dev-libs/lib-1"));
    CHECK_FALSE(app.plan_requested().has_value());
    FakeScreen screen{12, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(),
                   "! what the merge list waits for is unknown: no ebuild for dev-libs/lib-1"));
    // A flat list meanwhile, every package at the top.
    CHECK(contains(screen.text(), "  |-   o app-misc/plugin-1"));

    // The list changes as packages merge: a smaller one is still asked, being new.
    auto shorter = lib_merge_list();
    shorter.erase(shorter.begin());
    app.handle(key(KeyKind::tick));
    app.finish_watch({building_lib()}, {}, std::nullopt, shorter);
    REQUIRE(app.plan_requested().has_value());
    app.finish_plan(lib_waits);
    CHECK_FALSE(app.watched()->plan_error.has_value());
    // Known now; what merges drops out without asking again.
    shorter.erase(shorter.begin());
    app.handle(key(KeyKind::tick));
    app.finish_watch({building_lib()}, {}, std::nullopt, shorter);
    CHECK_FALSE(app.plan_requested().has_value());
    // No list, no plan, and nothing kept of the last one.
    app.handle(key(KeyKind::tick));
    app.finish_watch({building_lib()});
    CHECK(app.watched()->waits.empty());
}

TEST_CASE("run reads the merge list each second and plans it once") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{16, 120, {character(U'e'), key(KeyKind::tick), key(KeyKind::tick)}};
    int reads = 0;
    int plans = 0;
    egraph::tui::run(screen, app, ascii,
                     {.watch = [] { return std::vector{building_lib()}; },
                      .merge_list =
                          [&] {
                              ++reads;
                              return lib_merge_list();
                          },
                      .plan = [&](const std::vector<egraph::emerge::Pending>& list)
                          -> std::expected<egraph::emerge::Waits, std::string> {
                          ++plans;
                          CHECK(list.size() == 4);
                          return lib_waits;
                      },
                      .now = by_the_second(screen)});
    CHECK(reads == 3);
    CHECK(plans == 1);
    CHECK(contains(screen.text(), "waits for 1"));
    // Enter on a package that is only on the list opens nothing installed, and says so.
    app.handle(key(KeyKind::end));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "No version of app-doc/doc is installed yet");
}

TEST_CASE("with the evaluated store, pages read dependencies as emerge does") {
    for (const bool dynamic_deps : {true, false}) {
        egraph::tui::App app{both(), dynamic_deps};
        CHECK(app.has_evaluated());
        FakeScreen screen{24, 100, {key(KeyKind::enter)}};
        egraph::tui::run(screen, app, ascii, {.check = no_check});
        REQUIRE(app.pages().size() == 1);
        const auto text = screen.text();
        CHECK(contains(text, "dev-libs/b:=") == dynamic_deps);
        CHECK(contains(text, "dev-libs/b |") != dynamic_deps);
        // The installed store stays as built, for the check.
        CHECK(app.installed().nodes_in(app.installed().packages.at(0).deps.at(4)).size() == 4);
    }
}

TEST_CASE("the list opens on pending updates, and u shows them among the rest") {
    egraph::tui::App app{only_b_updates(), true};
    CHECK(app.list().only == egraph::tui::Only::updates);
    CHECK(app.list().shown == std::vector<std::uint32_t>{1});
    FakeScreen screen{10, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 updates"));
    CHECK(contains(screen.line(9), "u all"));
    app.handle(character(U'/'));
    app.handle(character(U'a'));
    app.handle(character(U'-'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "no package matches"));
    app.handle(key(KeyKind::escape));
    app.handle(character(U'u'));
    CHECK(app.list().only == egraph::tui::Only::all);
    CHECK(app.list().shown.size() == 2);
    egraph::tui::draw(screen, app, ascii);
    CHECK_FALSE(contains(screen.line(3), "  U "));
    CHECK(contains(screen.line(4), "dev-libs/b-1  U 2"));
    CHECK_FALSE(contains(screen.line(9), "u updates"));
}

TEST_CASE("the list says when nothing is pending") {
    egraph::test::Bytes dependencies;
    dependencies.varint(2);
    dependencies.varints({1, 0, 3}).varint(0);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(0).varint(0);
    dependencies.varints({1, 0, 0, 0}).list({}).varints({0, 0, 0, 0, 0, 0, 0, 0});
    dependencies.varints({2, 1, 3}).varint(0);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(0).varint(0);
    dependencies.varints({1, 0, 0, 0}).list({}).varints({0, 0, 0, 0, 0, 0, 0, 0});
    egraph::tui::App app{
        egraph::Stores{.installed = sample(), .evaluated = evaluated_sample(dependencies)}, true};
    FakeScreen screen{10, 120, {}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    CHECK(app.list().shown.empty());
    CHECK(contains(screen.text(), "nothing to update"));
}

TEST_CASE("without an evaluated store there is nothing to update") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    CHECK_FALSE(app.has_evaluated());
    FakeScreen screen{10, 120, {character(U'u')}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    CHECK(app.list().only == egraph::tui::Only::all);
    CHECK_FALSE(contains(screen.line(9), "updates"));
}

TEST_CASE("a page shows its pending update and what toggled flags would add") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{30, 120, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    auto text = screen.text();
    CHECK(contains(text, "Update"));
    CHECK(contains(text, "   R rebuild for flag* -new%  ::test_repo"));
    CHECK(contains(text, "Would depend on, with flags toggled  2"));
    CHECK(contains(text, "dev-libs/b  +flag"));
    CHECK(contains(text, "   ....B   dev-libs/gone |  +flag -minimal  not installed"));
    CHECK_FALSE(contains(text, "Would be needed by"));

    // Only what is installed can be opened: b-1, from the dependencies, above the flags.
    app.handle(key(KeyKind::end));
    for (int flags = 0;
         flags < 10 &&
         app.pages().back().rows.at(app.pages().back().cursor.at).type == RowType::flag;
         ++flags) {
        app.handle(key(KeyKind::up));
    }
    CHECK(app.pages().back().rows.at(app.pages().back().cursor.at).flags == "+flag");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(app.pages().back().package == 1);
    egraph::tui::draw(screen, app, ascii);
    text = screen.text();
    CHECK(contains(text, "   U upgrade to dev-libs/b-2  ::test_repo"));
    CHECK(contains(text, "Would be needed by, with flags toggled  1"));
    CHECK(contains(text, "app-misc/a-1"));
}

TEST_CASE("a page lists its package's USE, each flag with where it was last set") {
    auto system = egraph::test::make_system({{.cpv = "app-misc/a-1"}},
                                            {{.cpv = "app-misc/a-1", .iuse = "x y z"}});
    egraph::test::set_ledger(
        system.evaluated,
        {.conf = {{.file = "mc", .line = 3, .tokens = "x y"}},
         .package_use = {{.file = "pu", .line = 2, .atom = "app-misc/a", .tokens = "-x"}},
         .package_env = {{.file = "pe", .line = 1, .atom = "app-misc/a", .tokens = "e.conf"}},
         .env_files = {{"e.conf", {{.file = "env/e.conf", .tokens = ""}}}}});
    egraph::tui::App app{egraph::Stores{.installed = std::move(system.store),
                                        .evaluated = std::move(system.evaluated)},
                         true};
    FakeScreen screen{20, 80, {}};
    // Every package, not only those with updates.
    app.handle(character(U'u'));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(text, "USE  3"));
    CHECK(contains(text, "   package.env  e.conf"));
    CHECK(contains(text, "   -x  pu:2"));
    CHECK(contains(text, "   y   mc:3"));
    CHECK(contains(text, "   -z  not set"));

    // The cursor stops on flags.
    const auto& page = app.pages().back();
    std::vector<std::string> flags;
    for (const auto& row : page.rows) {
        if (row.type == RowType::flag && row.flag) {
            flags.push_back(row.flag->flag);
        }
    }
    CHECK(flags == std::vector<std::string>{"x", "y", "z"});
    app.handle(key(KeyKind::end));
    CHECK(page.rows.at(page.cursor.at).type == RowType::flag);
}

TEST_CASE("the check compares installed stores, and a preview keeps the evaluated one") {
    egraph::tui::App app{both(), true};
    std::size_t checked_nodes = 0;
    const egraph::tui::Checker check =
        checking([&](const egraph::Store& stored) -> egraph::tui::CheckResult {
            checked_nodes = stored.nodes_in(stored.packages.at(0).deps.at(4)).size();
            return egraph::tui::Fresh{
                .store = sample(), .evaluated = evaluated_sample(), .drift = {"~dev-libs/b-1"}};
        });
    FakeScreen screen{12, 120, {character(U'c'), character(U'u')}};
    egraph::tui::run(screen, app, ascii, {.check = check});
    CHECK(checked_nodes == 4);
    CHECK(app.source() == egraph::tui::Source::preview);
    CHECK(app.has_evaluated());
    CHECK(app.store().nodes_in(app.store().packages.at(0).deps.at(4)).size() == 1);
    CHECK(app.update_of(1).has_value());
}

namespace {

// Each character of text as a key.
std::deque<Key> typed(std::string_view text) {
    std::deque<Key> keys;
    for (const char c : text) {
        keys.push_back(character(static_cast<char32_t>(c)));
    }
    return keys;
}

} // namespace

TEST_CASE("a command typed at the prompt runs, and its output links to pages") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    auto keys = typed(":deps app-misc/a-1");
    keys.push_back(key(KeyKind::enter));
    std::vector<std::string> ran;
    const egraph::tui::Commander command = [&](const std::string& line) {
        ran.push_back(line);
        return egraph::tui::Answer{.exit = egraph::Exit::ok,
                                   .out =
                                       "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1\tany-of\n"
                                       "@selected\tapp-misc/a\n",
                                   .err = {}};
    };
    FakeScreen screen{12, 100, keys};
    egraph::tui::run(screen, app, ascii, {.command = command});
    CHECK(ran == std::vector<std::string>{"deps app-misc/a-1"});
    REQUIRE(app.output().has_value());
    CHECK(app.output()->rows.size() == 2);
    CHECK(app.output()->links == std::vector<std::optional<std::uint32_t>>{0, std::nullopt});
    const auto text = screen.text();
    CHECK(contains(screen.line(0), ":deps app-misc/a-1"));
    CHECK(contains(screen.line(0), "2 lines"));
    // Fields line up in columns.
    CHECK(contains(text, "app-misc/a-1  RDEPEND  dev-libs/b  dev-libs/b-1  any-of"));
    // Rows of another shape line up among themselves.
    CHECK(contains(text, "@selected  app-misc/a"));

    // Enter opens the linked package; Esc comes back to the output, then to the list.
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 0);
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.output().has_value());
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.output().has_value());
}

TEST_CASE("the prompt edits, draws and cancels") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    for (const auto& k : typed(":xy")) {
        app.handle(k);
    }
    app.handle(key(KeyKind::backspace));
    REQUIRE(app.prompt().has_value());
    CHECK(*app.prompt() == "x");
    FakeScreen screen{12, 60, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(screen.line(11).starts_with(":x"));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.prompt().has_value());
    CHECK_FALSE(app.command_requested().has_value());
    // An empty command runs nothing.
    app.handle(character(U':'));
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.prompt().has_value());
    CHECK_FALSE(app.command_requested().has_value());
    // While searching, : is part of the query.
    app.handle(character(U'/'));
    app.handle(character(U':'));
    CHECK_FALSE(app.prompt().has_value());
    CHECK(app.list().query == ":");
}

TEST_CASE("a command's errors and warnings show in a dialog") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    for (const auto& k : typed(":nope")) {
        app.handle(k);
    }
    app.handle(key(KeyKind::enter));
    REQUIRE(app.command_requested() == std::optional<std::string>{"nope"});
    app.finish_command({.exit = egraph::Exit::usage,
                        .out = {},
                        .err = "egraph: shell: nope: no such command (help lists them)\n"});
    CHECK_FALSE(app.command_requested().has_value());
    CHECK_FALSE(app.output().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->lines ==
          std::vector<std::string>{"egraph: shell: nope: no such command (help lists them)"});

    app.handle(character(U'x'));
    app.handle(character(U':'));
    app.handle(character(U'o'));
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok,
                        .out = "dev-libs/b-1\n",
                        .err = "egraph: warning: answering from a stale store (why)\n"});
    REQUIRE(app.output().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK_FALSE(app.dialog()->error);

    // quit at the prompt ends the interface.
    app.handle(character(U'x'));
    app.handle(character(U':'));
    app.handle(character(U'q'));
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok, .out = {}, .err = {}, .quit = true});
    CHECK(app.done());

    // A command that prints nothing says so.
    app.handle(character(U'x'));
    app.handle(character(U':'));
    app.handle(character(U'o'));
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok, .out = {}, .err = {}});
    REQUIRE(app.output().has_value());
    CHECK(app.output()->rows.empty());
    FakeScreen screen{12, 60, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "the command printed nothing"));
}

TEST_CASE("held updates show in the list, and a held package's page offers its remedies") {
    // skin holds rgb, and only world keeps it; kwin holds lazy, and panel needs kwin.
    auto system = egraph::test::make_system(
        {{.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "<dev-libs/lazy-2"}}},
         {.cpv = "app-misc/panel-1", .deps = {{"RDEPEND", "app-misc/kwin"}}},
         {.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<dev-libs/rgb-2"}}},
         {.cpv = "dev-libs/lazy-1"},
         {.cpv = "dev-libs/rgb-1"},
         {.cpv = "dev-libs/up-1"}},
        {{.cpv = "dev-libs/lazy-1"},
         {.cpv = "dev-libs/lazy-2"},
         {.cpv = "dev-libs/rgb-1"},
         {.cpv = "dev-libs/rgb-2"},
         {.cpv = "dev-libs/up-1"},
         {.cpv = "dev-libs/up-2"}},
        {"app-misc/panel", "app-misc/skin"});
    egraph::tui::App app{egraph::Stores{.installed = std::move(system.store),
                                        .evaluated = std::move(system.evaluated)},
                         true};
    FakeScreen screen{30, 120, {}};
    CHECK(app.list().shown == std::vector<std::uint32_t>{3, 4, 5});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "dev-libs/lazy-1  H held"));
    CHECK(contains(screen.text(), "dev-libs/up-1  U 2"));

    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 4);
    egraph::tui::draw(screen, app, ascii);
    auto text = screen.text();
    CHECK(contains(text, "Held back"));
    CHECK(contains(text, "   H upgrade to dev-libs/rgb-2  ::test_repo"));
    CHECK(contains(text, " > R....   app-misc/skin-1                           <dev-libs/rgb-2"));
    CHECK(contains(text, "nothing depends on it; only @selected keeps it"));
    CHECK(contains(text, "   to remove it: emerge --deselect app-misc/skin"));
    CHECK(contains(text, "                 emerge -C =app-misc/skin-1"));
    CHECK(contains(text, "                 emerge -1 =dev-libs/rgb-2"));
    CHECK(contains(text, "   to keep it:   emerge -1 --nodeps =dev-libs/rgb-2"));

    // The holder is a link to its own page.
    const auto& page = app.pages().back();
    const auto holder = std::ranges::find_if(page.rows, [](const egraph::tui::Row& row) {
        return row.type == RowType::link && row.link.package == 2;
    });
    REQUIRE(holder != page.rows.end());
    while (app.pages().back().cursor.at !=
           static_cast<std::size_t>(holder - app.pages().back().rows.begin())) {
        app.handle(key(KeyKind::down));
    }
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().back().package == 2);

    // A holder something else needs is only named.
    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::up));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().back().package == 3);
    egraph::tui::draw(screen, app, ascii);
    text = screen.text();
    CHECK(contains(text, "needed by app-misc/panel-1"));
    CHECK_FALSE(contains(text, "to remove"));
    CHECK(contains(text, "to keep it:"));
}

namespace {

std::shared_ptr<const egraph::Stores> shared(egraph::test::System system) {
    return std::make_shared<const egraph::Stores>(egraph::Stores{
        .installed = std::move(system.store), .evaluated = std::move(system.evaluated)});
}

// a/app-1 needing dev-libs/lib and x/other, lib at version.
egraph::test::System app_with_lib(const std::string& version, bool with_new = false) {
    std::vector<egraph::test::Installed> installed{
        {.cpv = "a/app-1", .deps = {{"RDEPEND", "dev-libs/lib x/other"}}},
        {.cpv = "dev-libs/lib-" + version}};
    if (with_new) {
        installed.push_back({.cpv = "x/new-1"});
    }
    installed.push_back({.cpv = "x/other-1"});
    std::vector<egraph::test::Available> available;
    for (const auto& pkg : installed) {
        available.push_back({.cpv = pkg.cpv});
    }
    return egraph::test::make_system(installed, available);
}

std::string cpv_of(const egraph::tui::App& app, std::uint32_t id) {
    return std::string{app.store().string(app.store().packages.at(id).cpv)};
}

// A clock that moves on by stale_interval with each key the screen reads.
egraph::tui::Clock ticking(const FakeScreen& screen) {
    return [&screen] {
        return std::chrono::steady_clock::time_point{} +
               egraph::tui::stale_interval * static_cast<int>(screen.timeouts.size());
    };
}

} // namespace

TEST_CASE("refreshed stores keep the list and pages where they were") {
    SECTION("the list's cursor stays on its package") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        app.handle(character(U'u'));
        app.handle(key(KeyKind::down));
        app.handle(key(KeyKind::down));
        REQUIRE(cpv_of(app, app.list().shown.at(app.list().cursor.at)) == "x/other-1");
        app.finish_stale_check("x/new-1 was installed");
        CHECK(app.refresh_requested());
        app.finish_refresh(shared(app_with_lib("1", true)));
        CHECK_FALSE(app.stale());
        CHECK(app.store().packages.size() == 4);
        CHECK(app.list().only == egraph::tui::Only::all);
        CHECK(app.list().cursor.at == 3);
        CHECK(cpv_of(app, app.list().shown.at(app.list().cursor.at)) == "x/other-1");
    }
    SECTION("a page stays open, on the row it was on, across an upgrade") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        app.handle(character(U'u'));
        app.handle(key(KeyKind::enter));
        REQUIRE(app.pages().size() == 1);
        const auto& before = app.pages().back();
        REQUIRE(before.rows.at(before.cursor.at).type == RowType::link);
        REQUIRE(cpv_of(app, before.rows.at(before.cursor.at).link.package) == "dev-libs/lib-1");
        app.handle(key(KeyKind::enter));
        REQUIRE(app.pages().size() == 2);

        app.finish_refresh(shared(app_with_lib("2")));
        REQUIRE(app.pages().size() == 2);
        const auto& page = app.pages().front();
        CHECK(cpv_of(app, page.package) == "a/app-1");
        CHECK(page.rows.at(page.cursor.at).type == RowType::link);
        CHECK(cpv_of(app, page.rows.at(page.cursor.at).link.package) == "dev-libs/lib-2");
        // The upgraded package's own page follows it to the new version.
        CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-2");
    }
}

TEST_CASE("the stores' inputs are looked at while idle, and refreshed in the background") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{
        12, 120, {key(KeyKind::tick), key(KeyKind::tick), key(KeyKind::tick), key(KeyKind::tick)}};
    int looks = 0;
    int refreshes = 0;
    std::string refreshing;
    const egraph::tui::Services services{
        .stale = [&](const egraph::Stores& stores) -> std::optional<std::string> {
            CHECK(&stores == app.shared().get());
            return ++looks == 1 ? std::optional<std::string>{"x/new-1 was installed"}
                                : std::nullopt;
        },
        .refresh = [&]() -> egraph::Job<egraph::tui::RefreshResult> {
            ++refreshes;
            return [&, polls = 0]() mutable -> std::optional<egraph::tui::RefreshResult> {
                if (++polls < 2) {
                    return std::nullopt;
                }
                refreshing = screen.line(0);
                return shared(app_with_lib("1", true));
            };
        },
        .now = ticking(screen)};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(looks >= 2);
    CHECK(refreshes == 1);
    CHECK(app.store().packages.size() == 4);
    CHECK(contains(refreshing, "/ refreshing"));
    CHECK_FALSE(contains(screen.line(0), "refreshing"));
    REQUIRE(screen.timeouts.size() >= 2);
    // Idle, it wakes when the next look is due; refreshing, it polls the build.
    CHECK(screen.timeouts.at(0) == egraph::tui::stale_interval);
    CHECK(screen.timeouts.at(1) == egraph::tui::wait_interval);
}

TEST_CASE("a failed refresh says why and waits before trying again") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{12, 120, {key(KeyKind::tick), key(KeyKind::tick)}};
    int refreshes = 0;
    const egraph::tui::Services services{
        .stale = [](const egraph::Stores&) -> std::optional<std::string> { return "changed"; },
        .refresh = [&]() -> egraph::Job<egraph::tui::RefreshResult> {
            ++refreshes;
            return egraph::ready(egraph::tui::RefreshResult{
                std::unexpected("egraph-build exited with status 1:\nTraceback")});
        },
        .now = ticking(screen)};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(refreshes == 1);
    CHECK(app.store().packages.size() == 3);
    REQUIRE(app.refresh_error().has_value());
    INFO(screen.line(0));
    CHECK(screen.line(0).ends_with(" refresh failed: egraph-build exited with status 1 "));
    CHECK(std::ranges::find(screen.timeouts, egraph::tui::retry_interval) != screen.timeouts.end());
}

TEST_CASE("no refresh runs while the check view is open") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{12,
                      120,
                      {character(U'c'), key(KeyKind::tick), key(KeyKind::tick), key(KeyKind::tick),
                       key(KeyKind::escape), key(KeyKind::tick)}};
    int refreshes = 0;
    const egraph::tui::Services services{
        .check = cyclic_check,
        .stale = [&](const egraph::Stores&) -> std::optional<std::string> {
            return refreshes == 0 ? std::optional<std::string>{"changed"} : std::nullopt;
        },
        .refresh = [&]() -> egraph::Job<egraph::tui::RefreshResult> {
            ++refreshes;
            CHECK_FALSE(app.checked().has_value());
            return egraph::ready(egraph::tui::RefreshResult{shared(app_with_lib("1", true))});
        },
        .now = ticking(screen)};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(refreshes == 1);
    CHECK(app.store().packages.size() == 4);
}

namespace {

// top needs glibmm, whose next version pulls in mm-common and through it chain; only world keeps
// top, and nothing keeps loose, installed at loose_version.
egraph::test::System glibmm_system(const std::string& loose_version = "1") {
    return egraph::test::make_system(
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/loose-" + loose_version},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}}},
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/glibmm-2", .deps = {{"BDEPEND", "dev-cpp/mm-common"}}},
         {.cpv = "app-misc/loose-1"},
         {.cpv = "app-misc/loose-2"},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}},
         {.cpv = "dev-cpp/mm-common-1", .deps = {{"RDEPEND", "dev-libs/chain"}}, .iuse = "doc"},
         {.cpv = "dev-libs/chain-1"}},
        {"app-misc/top"});
}

// Each row as its depth, label, and the merge's candidate cpv where it is one.
std::vector<std::string> described(const egraph::tui::App& app) {
    std::vector<std::string> out;
    for (const auto& row : app.planned()->rows) {
        auto line = std::format("{} {}", row.depth, row.label);
        if (row.merge) {
            const auto& evaluated = app.evaluated();
            line += std::format(
                " -> {}",
                evaluated.string(
                    evaluated.candidates.at(app.plan().merges.at(*row.merge).candidate).cpv));
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::string selected_label(const egraph::tui::App& app) {
    const auto& planned = *app.planned();
    return planned.rows.at(planned.cursor.at).label;
}

} // namespace

TEST_CASE("p shows the plan as a tree under each root set") {
    egraph::tui::App app{shared(glibmm_system()), true};
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK_FALSE(contains(screen.line(15), "p plan"));
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    CHECK(described(app) == std::vector<std::string>{
                                "0 ",
                                "1 app-misc/loose-1 -> app-misc/loose-2",
                                "0 @selected",
                                "1 app-misc/top-1",
                                "2 app-misc/glibmm-1 -> app-misc/glibmm-2",
                                "3 dev-cpp/mm-common-1 -> dev-cpp/mm-common-1",
                                "4 dev-libs/chain-1 -> dev-libs/chain-1",
                            });
    CHECK(selected_label(app) == "app-misc/loose-1");
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(screen.line(0), "plan  4 merges"));
    CHECK(contains(text, "\n   - nothing keeps"));
    CHECK(contains(text, "\n > `- U app-misc/loose  1 > 2  ::test_repo  1     "));
    CHECK(contains(text, "\n   @ @selected"));
    CHECK(contains(text, "\n   `- app-misc/top-1"));
    CHECK(contains(text, "\n     `- U app-misc/glibmm  1 > 2  ::test_repo  4  w 3"));
    CHECK(contains(text, "\n       `- N dev-cpp/mm-common  1  ::test_repo  3  w 2"));
    CHECK(contains(text, "\n         `- N dev-libs/chain  1  ::test_repo  2"));
    CHECK(contains(screen.line(15), "esc back"));
}

TEST_CASE("the plan view names a new slot's installed neighbours") {
    egraph::tui::App app{shared(egraph::test::make_system(
                             {{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "dev-lang/py:1"}}},
                              {.cpv = "dev-lang/py-1", .slot = "1"}},
                             {{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "dev-lang/py:1"}}},
                              {.cpv = "app-misc/top-2", .deps = {{"RDEPEND", "dev-lang/py:2"}}},
                              {.cpv = "dev-lang/py-1", .slot = "1"},
                              {.cpv = "dev-lang/py-2", .slot = "2"}},
                             {"app-misc/top"})),
                         true};
    FakeScreen screen{16, 120, {}};
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "N dev-lang/py  2  ::test_repo  1  beside 1:1"));
}

TEST_CASE("the plan view moves over packages and opens the installed ones") {
    egraph::tui::App app{shared(glibmm_system()), true};
    app.handle(character(U'p'));
    // Past the set to its first package.
    app.handle(key(KeyKind::down));
    CHECK(selected_label(app) == "app-misc/top-1");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "app-misc/top-1");
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    REQUIRE(app.planned().has_value());

    // A replacement opens the page of what it replaces.
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "app-misc/glibmm-1");
    app.handle(key(KeyKind::escape));

    // A new package has no page, only what pulls it in and its USE.
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "dev-cpp/mm-common-1 is not installed");
    CHECK(
        app.dialog()->lines ==
        std::vector<std::string>{"The plan pulls it in for app-misc/glibmm-2's dev-cpp/mm-common.",
                                 "It would be built with USE=\"-doc\"."});
    app.handle(key(KeyKind::escape));

    app.handle(key(KeyKind::home));
    CHECK(selected_label(app) == "app-misc/loose-1");
    app.handle(key(KeyKind::end));
    CHECK(selected_label(app) == "dev-libs/chain-1");
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.planned().has_value());
    CHECK_FALSE(app.done());
    app.handle(character(U'p'));
    app.handle(character(U'q'));
    CHECK(app.done());
}

TEST_CASE("the plan view says when nothing is to merge, and needs the evaluated store") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    CHECK(app.planned()->rows.empty());
    FakeScreen screen{10, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "nothing to merge"));

    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App bare{store, graph};
    bare.handle(character(U'p'));
    CHECK_FALSE(bare.planned().has_value());
}

TEST_CASE("refreshed stores keep the plan view on its package") {
    egraph::tui::App app{shared(glibmm_system()), true};
    app.handle(character(U'p'));
    app.handle(key(KeyKind::down));
    REQUIRE(selected_label(app) == "app-misc/top-1");
    app.finish_refresh(shared(glibmm_system("2")));
    REQUIRE(app.planned().has_value());
    CHECK(app.planned()->rows.size() == 5);
    CHECK(selected_label(app) == "app-misc/top-1");

    // What it was on is gone: the same place, on a package.
    app.handle(key(KeyKind::end));
    app.finish_refresh(shared(egraph::test::make_system(
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}}},
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/glibmm-2"},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}}},
        {"app-misc/top"})));
    REQUIRE(app.planned().has_value());
    CHECK(app.planned()->rows.size() == 3);
    CHECK(selected_label(app) == "app-misc/glibmm-1");
}

namespace {

// dev-libs/lib at 1 (installed with app_with_lib("1")), 2 and ~x86 3; app-misc/new-tool at 1.0
// and ~x86 2.0, not installed.
std::shared_ptr<const egraph::RepositoryIndex> repository_index() {
    egraph::test::IndexBuilder b{{"test_repo", "overlay"}};
    for (const auto* version : {"1", "2"}) {
        b.version({.cpv = std::format("dev-libs/lib-{}", version),
                   .repo = "test_repo",
                   .description = "A library"});
    }
    b.version({.cpv = "dev-libs/lib-3", .keywords = "~x86", .repo = "test_repo"});
    b.version({.cpv = "app-misc/new-tool-1.0",
               .license = "MIT",
               .repo = "overlay",
               .description = "A new tool",
               .homepage = "https://example.org"});
    b.version({.cpv = "app-misc/new-tool-2.0", .keywords = "~x86", .repo = "overlay"});
    return std::make_shared<const egraph::RepositoryIndex>(b.index());
}

void type(egraph::tui::App& app, std::string_view text) {
    for (const char c : text) {
        app.handle(character(static_cast<char32_t>(c)));
    }
}

std::vector<std::string> result_cps(const egraph::tui::App& app) {
    std::vector<std::string> cps;
    for (const auto& found : app.search()->results) {
        cps.push_back(found.cp);
    }
    return cps;
}

} // namespace

TEST_CASE("s searches the repositories, once the index is loaded") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    REQUIRE(app.search().has_value());
    CHECK(app.search()->typing);
    CHECK(app.index_requested());
    // : is part of the key while typing.
    type(app, "tool:");
    CHECK_FALSE(app.prompt().has_value());
    app.handle(key(KeyKind::backspace));
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.search()->typing);
    CHECK(app.search()->pending);
    CHECK(app.search()->results.empty());

    app.finish_index(repository_index());
    CHECK_FALSE(app.index_requested());
    REQUIRE(app.catalogue().has_value());
    CHECK_FALSE(app.search()->pending);
    CHECK(app.search()->ran == "tool");
    REQUIRE(result_cps(app) == std::vector<std::string>{"app-misc/new-tool"});
    const auto& found = app.search()->results.front();
    CHECK(found.version == "1.0");
    CHECK(found.visible);
    CHECK(found.installed.empty());
    CHECK(found.description == "A new tool");

    // A new search, with the index already there.
    app.handle(character(U'/'));
    CHECK(app.search()->typing);
    for (int i = 0; i < 4; ++i) {
        app.handle(key(KeyKind::backspace));
    }
    type(app, "lib");
    app.handle(key(KeyKind::enter));
    CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
    CHECK(app.search()->results.front().installed == "1");
    CHECK(app.search()->results.front().version == "2");
}

TEST_CASE("tab searches descriptions too, and esc before any search closes it") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.search().has_value());

    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "library");
    app.handle(key(KeyKind::enter));
    CHECK(app.search()->results.empty());
    app.handle(key(KeyKind::tab));
    CHECK(app.search()->descriptions);
    CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
    // Esc after a search stops typing but keeps the results; again, back to the list.
    app.handle(character(U'/'));
    app.handle(key(KeyKind::escape));
    CHECK(app.search().has_value());
    CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.search().has_value());
}

TEST_CASE("an installed result opens its page, with the versions in the repositories") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "lib");
    app.handle(key(KeyKind::enter));
    // Drawn first, as run() does, so the page knows its height.
    FakeScreen screen{30, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-1");
    std::vector<std::string> versions;
    for (const auto& row : app.pages().back().rows) {
        if (row.type == RowType::version) {
            REQUIRE(row.version.has_value());
            versions.push_back(row.version->version);
        }
    }
    CHECK(versions == std::vector<std::string>{"1", "2", "3"});

    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "Versions  3"));
    CHECK(contains(text, "::test_repo"));
    CHECK(contains(text, "masked: ~x86 keyword"));

    // Last on the page, after the dependencies, and the cursor reaches them.
    const auto& rows = app.pages().back().rows;
    CHECK(rows.back().type == RowType::version);
    CHECK(rows.at(app.pages().back().cursor.at).type == RowType::link);
    app.handle(character(U'G'));
    CHECK(app.pages().back().cursor.at == rows.size() - 1);
    egraph::tui::draw(screen, app, ascii);
    // The cursor on the last version, 3.
    CHECK(contains(screen.text(), ">   3 "));
    app.handle(key(KeyKind::up));
    app.handle(key(KeyKind::up));
    CHECK(rows.at(app.pages().back().cursor.at).version->version == "1");
    // This very version: nothing to open.
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().size() == 1);

    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.search().has_value());
}

TEST_CASE("enter on another installed version opens its page") {
    // dev-libs/lib in two slots, both installed.
    auto system = egraph::test::make_system(
        {{.cpv = "dev-libs/lib-1", .slot = "1"}, {.cpv = "dev-libs/lib-2", .slot = "2"}},
        {{.cpv = "dev-libs/lib-1", .slot = "1"}, {.cpv = "dev-libs/lib-2", .slot = "2"}});
    egraph::tui::App app{shared(std::move(system)), false};
    egraph::test::IndexBuilder b{{"test_repo"}};
    b.version({.cpv = "dev-libs/lib-1", .slot = "1", .repo = "test_repo"});
    b.version({.cpv = "dev-libs/lib-2", .slot = "2", .repo = "test_repo"});
    app.finish_index(std::make_shared<const egraph::RepositoryIndex>(b.index()));
    FakeScreen screen{30, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(character(U'u'));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    REQUIRE(cpv_of(app, app.pages().back().package) == "dev-libs/lib-1");
    app.handle(character(U'G'));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-2");
}

TEST_CASE("a result not installed opens a listing of its ebuild and versions") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    FakeScreen screen{20, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.listing().has_value());
    const auto& listing = *app.listing();
    CHECK(listing.found.cp == "app-misc/new-tool");
    std::size_t versions = 0;
    for (const auto& row : listing.rows) {
        versions += row.type == RowType::version ? 1 : 0;
    }
    CHECK(versions == 2);
    REQUIRE(listing.cursor.at < listing.rows.size());
    CHECK(listing.rows.at(listing.cursor.at).type == RowType::version);

    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "search > new-tool"));
    CHECK(contains(text, "app-misc/new-tool  not installed"));
    CHECK(contains(text, "A new tool"));
    CHECK(contains(text, "https://example.org"));
    CHECK(contains(text, "MIT"));
    CHECK(contains(text, "::overlay"));
    CHECK(contains(text, "masked: ~x86 keyword"));

    // On the version the search shows, the best visible one.
    CHECK(listing.rows.at(listing.cursor.at).version->version == "1.0");
    app.handle(key(KeyKind::down));
    CHECK(listing.rows.at(listing.cursor.at).version->version == "2.0");
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.listing().has_value());
    CHECK(app.search().has_value());
}

TEST_CASE("the search view shows its results, and a wait while the index loads") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{12, 120, {}};
    app.handle(character(U's'));
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Loading the repository index"));
    app.finish_index(repository_index());
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "search  1 found"));
    CHECK(contains(text, "app-misc/new-tool"));
    CHECK(contains(text, "1.0"));
    CHECK(contains(text, "A new tool"));
}

TEST_CASE("run loads the index in the background, and a failure says why") {
    SECTION("loaded") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        FakeScreen screen{12,
                          120,
                          {character(U's'), character(U'l'), character(U'i'), character(U'b'),
                           key(KeyKind::enter), key(KeyKind::tick), key(KeyKind::tick)}};
        int loads = 0;
        std::string waiting;
        const egraph::tui::Services services{
            .load_index = [&]() -> egraph::Job<egraph::tui::IndexResult> {
                ++loads;
                return [&, polls = 0]() mutable -> std::optional<egraph::tui::IndexResult> {
                    if (++polls < 3) {
                        return std::nullopt;
                    }
                    waiting = screen.text();
                    return repository_index();
                };
            }};
        egraph::tui::run(screen, app, ascii, services);
        CHECK(loads == 1);
        CHECK(contains(waiting, "Loading the repository index"));
        REQUIRE(app.search().has_value());
        CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
        CHECK(std::ranges::find(screen.timeouts, egraph::tui::wait_interval) !=
              screen.timeouts.end());
    }
    SECTION("failed") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        FakeScreen screen{12, 120, {character(U's'), key(KeyKind::tick)}};
        int loads = 0;
        const egraph::tui::Services services{
            .load_index = [&]() -> egraph::Job<egraph::tui::IndexResult> {
                ++loads;
                return egraph::ready(
                    egraph::tui::IndexResult{std::unexpected("egraph-build exited with status 1")});
            }};
        egraph::tui::run(screen, app, ascii, services);
        CHECK(loads == 1);
        CHECK_FALSE(app.index_requested());
        REQUIRE(app.dialog().has_value());
        CHECK(app.dialog()->title == "The repository index could not be loaded");
        // Opening the search again tries again.
        app.handle(key(KeyKind::escape));
        app.handle(key(KeyKind::escape));
        app.handle(character(U's'));
        CHECK(app.index_requested());
    }
}

TEST_CASE("refreshed stores keep the search, its results now as installed") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "lib");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.search()->results.front().installed == "1");
    app.finish_refresh(shared(app_with_lib("2")));
    REQUIRE(app.search().has_value());
    CHECK(app.search()->results.front().installed == "2");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-2");
}

TEST_CASE("a command's output from the search replaces a listing over it") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.listing().has_value());
    app.handle(character(U':'));
    type(app, "orphans");
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok, .out = "x/other-1\n", .err = {}, .quit = false});
    CHECK_FALSE(app.listing().has_value());
    REQUIRE(app.output().has_value());
    app.handle(key(KeyKind::escape));
    CHECK(app.search().has_value());
}

TEST_CASE("an action's command: exec for merges, remove for removals") {
    using egraph::tui::Action;
    using Words = std::vector<std::string>;
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::install, .targets = {"a/b"}}) ==
          Words{"exec", "a/b"});
    CHECK(egraph::tui::action_arguments(
              {.kind = Action::Kind::update, .targets = {"a/b:0", "c/d:2"}}) ==
          Words{"exec", "--oneshot", "-u", "-N", "a/b:0", "c/d:2"});
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::update}) ==
          Words{"exec", "--oneshot", "-u", "-N", "-D", "@installed"});
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::remove, .targets = {"=a/b-1"}}) ==
          Words{"remove", "=a/b-1"});
    CHECK(egraph::tui::action_arguments(
              {.kind = Action::Kind::remove, .targets = {"=a/b-1"}, .build_deps = false}) ==
          Words{"remove", "--with-bdeps", "n", "=a/b-1"});
    CHECK(egraph::tui::action_arguments(
              {.kind = Action::Kind::rebuild, .targets = {"@preserved-rebuild"}}) ==
          Words{"exec", "--oneshot", "@preserved-rebuild"});
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::sync, .targets = {"gentoo"}}) ==
          Words{"sync", "gentoo"});
    CHECK(egraph::tui::previewed({.kind = Action::Kind::rebuild}));
    CHECK_FALSE(egraph::tui::previewed({.kind = Action::Kind::sync}));
}

TEST_CASE("enter on a notice updates, rebuilds or syncs what it is about") {
    using egraph::Notice;
    using egraph::NoticeKind;
    using egraph::tui::Action;
    using egraph::tui::notice_action;
    using egraph::tui::notice_work;
    const Notice glsa{.kind = NoticeKind::glsa,
                      .key = "glsa:202609-03",
                      .title = "",
                      .detail = {},
                      .packages = {"dev-libs/b-1", "dev-libs/b-1.5", "app-misc/c-2"},
                      .fingerprint = "",
                      .since = {}};
    // Each package once, whichever of its versions are affected.
    CHECK(notice_action(glsa) ==
          Action{.kind = Action::Kind::update, .targets = {"dev-libs/b", "app-misc/c"}});
    CHECK(notice_work(glsa) == "update");
    const Notice missing{.kind = NoticeKind::missing,
                         .key = "missing:dev-libs/b-1",
                         .title = "",
                         .detail = {},
                         .packages = {"dev-libs/b-1"},
                         .fingerprint = "",
                         .since = {}};
    CHECK(notice_action(missing) ==
          Action{.kind = Action::Kind::rebuild, .targets = {"=dev-libs/b-1"}});
    CHECK(notice_work(missing) == "rebuild");
    const Notice preserved{.kind = NoticeKind::preserved, .key = "preserved"};
    CHECK(notice_action(preserved) ==
          Action{.kind = Action::Kind::rebuild, .targets = {"@preserved-rebuild"}});
    CHECK(notice_work(preserved) == "rebuild");
    const Notice stale{.kind = NoticeKind::stale, .key = "stale:gentoo"};
    CHECK(notice_action(stale) == Action{.kind = Action::Kind::sync, .targets = {"gentoo"}});
    CHECK(notice_work(stale) == "sync");
    // Opened as a page instead.
    const Notice masked{
        .kind = NoticeKind::masked, .key = "masked:dev-libs/b-1", .packages = {"dev-libs/b-1"}};
    CHECK_FALSE(notice_action(masked).has_value());
    CHECK(notice_work(masked) == "open");
    // A notices file from before notices named their packages.
    CHECK_FALSE(notice_action(Notice{.kind = NoticeKind::glsa, .key = "glsa:1"}).has_value());
    CHECK(notice_work(Notice{.kind = NoticeKind::glsa, .key = "glsa:1"}).empty());
    CHECK(notice_work(Notice{.kind = NoticeKind::news, .key = "news:gentoo/x"}).empty());
    // Handed to dispatch-conf instead.
    const Notice config{.kind = NoticeKind::config, .key = "config"};
    CHECK_FALSE(notice_action(config).has_value());
    CHECK(notice_work(config) == "dispatch-conf");
    // Run as :config check.
    const Notice check{.kind = NoticeKind::check, .key = "check"};
    CHECK_FALSE(notice_action(check).has_value());
    CHECK(notice_work(check) == "check");
    // The plan a configuration edit changed, as watch plans it.
    const Notice plan{.kind = NoticeKind::plan, .key = "plan"};
    CHECK(notice_action(plan) ==
          Action{.kind = Action::Kind::update, .targets = {}, .scope = egraph::tui::Scope::world});
    CHECK(notice_work(plan) == "update");
}

TEST_CASE("a log's tail is what a terminal would leave of its last lines") {
    using Lines = std::vector<std::string>;
    CHECK(egraph::tui::plain_tail("a\nb\nc\nd\n", 2) == Lines{"c", "d"});
    CHECK(egraph::tui::plain_tail("a\nb", 5) == Lines{"a", "b"});
    // Colours, titles, progress redrawn over itself, and the blank lines at the end.
    CHECK(egraph::tui::plain_tail("\x1b[32;01m * \x1b[39;49;00mok\r\n\x1b]0;title\a10%\r50%\r"
                                  "100%\n\x1b]2;t\x1b\\x\ty\x07\n\n  \n",
                                  10) == Lines{" * ok", "100%", "x       y"});
    CHECK(egraph::tui::plain_tail("", 3).empty());
    CHECK(egraph::tui::plain_tail("cut off \x1b[", 3) == Lines{"cut off "});
}

namespace {

using egraph::tui::Action;

std::string slot_atom(const egraph::tui::App& app, std::uint32_t id) {
    const auto& pkg = app.store().packages.at(id);
    return std::format("{}:{}", app.store().string(pkg.cp), app.store().string(pkg.slot));
}

// Previews the requested action as ready to run, and answers y.
void confirm(egraph::tui::App& app) {
    REQUIRE(app.preview_requested().has_value());
    app.finish_preview({.ready = true, .out = "the plan\n\n", .err = ""});
    app.handle(character(U'y'));
}

} // namespace

TEST_CASE("space picks packages in the list, and U updates them, or every update") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{12, 160, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(character(U' '));
    CHECK(app.picked() == std::vector<std::string>{"app-misc/a-1"});
    CHECK(app.list().cursor.at == 1);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 picked"));
    CHECK(screen.line(3).starts_with("  +"));
    CHECK(screen.line(4).starts_with(" > "));
    app.handle(character(U' '));
    CHECK(app.picked().size() == 2);
    // At the last row, the cursor stays to take the pick back.
    app.handle(character(U' '));
    CHECK(app.picked() == std::vector<std::string>{"app-misc/a-1"});

    app.handle(character(U'U'));
    REQUIRE(app.preview_requested().has_value());
    CHECK(*app.preview_requested() ==
          Action{.kind = Action::Kind::update, .targets = {slot_atom(app, 0)}, .build_deps = true});
    app.finish_preview({.ready = true, .out = "the plan\n\n", .err = ""});
    CHECK_FALSE(app.preview_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->question);
    CHECK(app.dialog()->title == "Run egraph exec?");
    CHECK(app.dialog()->lines == std::vector<std::string>{"the plan"});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "y yes  n no"));
    // Any other key leaves the question up.
    app.handle(character(U'x'));
    CHECK(app.dialog().has_value());
    app.handle(character(U'n'));
    CHECK_FALSE(app.dialog().has_value());
    CHECK_FALSE(app.run_requested().has_value());
    CHECK(app.picked().size() == 1);

    app.handle(key(KeyKind::home));
    app.handle(character(U' '));
    CHECK(app.picked().empty());
    app.handle(character(U'U'));
    CHECK(*app.preview_requested() ==
          Action{.kind = Action::Kind::update, .targets = {}, .build_deps = true});
}

TEST_CASE("a preview with nothing to do, or that cannot run, only says so") {
    egraph::tui::App app{both(), true};
    app.handle(character(U'U'));
    app.finish_preview({.ready = false, .out = "Nothing to merge.\n", .err = ""});
    REQUIRE(app.dialog().has_value());
    CHECK_FALSE(app.dialog()->error);
    CHECK_FALSE(app.dialog()->question);
    CHECK(app.dialog()->title == "Nothing to do");
    app.handle(character(U'y'));
    CHECK_FALSE(app.run_requested().has_value());

    app.handle(character(U'U'));
    app.finish_preview(
        {.ready = false,
         .out = "the plan\n",
         .err = "egraph: exec: merging needs write access to /var/db/pkg; run egraph as root\n"});
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->title == "It cannot run");
    CHECK(app.dialog()->lines ==
          std::vector<std::string>{
              "the plan", "",
              "egraph: exec: merging needs write access to /var/db/pkg; run egraph as root"});
}

TEST_CASE("a confirmed action runs in the emerge view, one at a time, and outlives quitting") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{12, 160, {}};
    app.handle(character(U' '));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    app.handle(key(KeyKind::escape));
    app.handle(character(U'U'));
    confirm(app);
    REQUIRE(app.run_requested().has_value());
    CHECK(app.run_requested()->kind == Action::Kind::update);
    CHECK(app.picked().empty());
    CHECK(app.watched().has_value());
    CHECK(app.refresh() == egraph::tui::wait_interval);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "egraph exec"));

    app.handle(key(KeyKind::escape));
    app.handle(character(U'U'));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "A run is going");
    CHECK_FALSE(app.preview_requested().has_value());
    app.handle(character(U'x'));

    app.handle(character(U'q'));
    CHECK_FALSE(app.done());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->question);
    app.handle(character(U'n'));
    CHECK_FALSE(app.done());
    app.handle(character(U'q'));
    app.handle(character(U'y'));
    CHECK(app.done());
}

TEST_CASE("a finished run says so, and a failed one shows the end of each failure's log") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{12, 160, {}};
    app.handle(character(U'U'));
    confirm(app);
    app.finish_run(egraph::tui::RunResult{});
    CHECK_FALSE(app.run_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK_FALSE(app.dialog()->error);
    CHECK(app.dialog()->title == "egraph exec finished");
    app.handle(character(U'x'));

    app.handle(key(KeyKind::escape));
    app.handle(character(U'U'));
    confirm(app);
    std::vector<std::string> tail;
    for (int i = 1; i <= 12; ++i) {
        tail.push_back(std::format("line {}", i));
    }
    app.finish_run(egraph::tui::RunResult{
        .status = 1,
        .failures = {{.cpv = "dev-libs/b-2", .log = "/var/tmp/b.log", .tail = tail},
                     {.cpv = "app-misc/a-1", .log = "", .tail = {}}},
        .output = "/var/lib/egraph/interface.log",
        .tail = {"egraph: exec: dev-libs/b-2: compile failed"}});
    REQUIRE(app.dialog().has_value());
    const auto& dialog = *app.dialog();
    CHECK(dialog.error);
    CHECK(dialog.title == "egraph exec failed");
    REQUIRE(dialog.lines.size() == 18);
    CHECK(dialog.lines.at(0) == "dev-libs/b-2 failed; its log, /var/tmp/b.log:");
    CHECK(dialog.lines.at(1) == "  line 1");
    CHECK(dialog.lines.at(14) == "app-misc/a-1 failed");
    CHECK(dialog.lines.at(16) ==
          "egraph exec exited with status 1; its output, /var/lib/egraph/interface.log:");
    CHECK(dialog.lines.at(17) == "  egraph: exec: dev-libs/b-2: compile failed");

    // Taller than the screen: the move keys scroll it, any other key closes it.
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "scroll any key"));
    CHECK(contains(screen.text(), "dev-libs/b-2 failed"));
    app.handle(key(KeyKind::down));
    CHECK(app.dialog()->top == 1);
    app.handle(character(U'G'));
    CHECK(app.dialog()->top == 18 - 8);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "compile failed"));
    CHECK_FALSE(contains(screen.text(), "dev-libs/b-2 failed;"));
    app.handle(key(KeyKind::page_up));
    CHECK(app.dialog()->top == 2);
    app.handle(character(U'g'));
    CHECK(app.dialog()->top == 0);
    app.handle(character(U'x'));
    CHECK_FALSE(app.dialog().has_value());
}

TEST_CASE("a run that cannot start says why") {
    egraph::tui::App app{both(), true};
    app.handle(character(U'U'));
    confirm(app);
    app.finish_run({.error = "egraph: No such file or directory"});
    CHECK_FALSE(app.run_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->title == "egraph exec could not start");
    CHECK(app.dialog()->lines == std::vector<std::string>{"egraph: No such file or directory"});
}

TEST_CASE("r removes the picked packages, or every orphan shown") {
    const auto store = build_only();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'r'));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "Nothing picked to remove");
    app.handle(character(U'x'));

    app.handle(character(U'b'));
    app.handle(character(U'o'));
    app.handle(character(U'r'));
    REQUIRE(app.preview_requested().has_value());
    CHECK(*app.preview_requested() ==
          Action{.kind = Action::Kind::remove, .targets = {"=dev-libs/b-1"}, .build_deps = false});
    app.finish_preview({});
    app.handle(character(U'x'));

    app.handle(character(U'o'));
    app.handle(character(U' '));
    app.handle(character(U'b'));
    app.handle(character(U'r'));
    CHECK(*app.preview_requested() ==
          Action{.kind = Action::Kind::remove, .targets = {"=app-misc/a-1"}, .build_deps = true});
}

TEST_CASE("i installs a search result, a listing's version, or a page's") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    app.handle(character(U'i'));
    REQUIRE(app.preview_requested().has_value());
    CHECK(*app.preview_requested() == Action{.kind = Action::Kind::install,
                                             .targets = {"app-misc/new-tool"},
                                             .build_deps = true});
    app.finish_preview({});
    app.handle(character(U'x'));

    app.handle(key(KeyKind::enter));
    REQUIRE(app.listing().has_value());
    app.handle(character(U'i'));
    CHECK(app.preview_requested()->targets ==
          std::vector<std::string>{"=app-misc/new-tool-1.0::overlay"});
    app.finish_preview({});
    app.handle(character(U'x'));

    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::escape));
    // dev-libs/lib-1's page, its versions last.
    app.handle(character(U'u'));
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    app.handle(character(U'G'));
    app.handle(character(U'i'));
    CHECK(app.preview_requested()->targets ==
          std::vector<std::string>{"=dev-libs/lib-3::test_repo"});
}

TEST_CASE("i on a version no repository holds says it cannot be installed") {
    // x/other-1 is installed, but the index has no ebuild of it.
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    app.handle(key(KeyKind::escape));
    app.handle(character(U'u'));
    app.handle(character(U'G'));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    REQUIRE(cpv_of(app, app.pages().back().package) == "x/other-1");
    app.handle(character(U'G'));
    app.handle(character(U'i'));
    CHECK_FALSE(app.preview_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "No repository holds x/other-1");
}

TEST_CASE("run previews an action once the screen says it plans, then runs it to its end") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{12, 160, {character(U'U'), character(U'y'), key(KeyKind::tick)}};
    std::string planning;
    std::vector<Action> runs;
    const egraph::tui::Services services{
        .preview =
            [&](const Action&) {
                planning = screen.text();
                return egraph::tui::Preview{.ready = true, .out = "the plan", .err = ""};
            },
        .run = [&](const Action& action) -> egraph::Job<egraph::tui::RunResult> {
            runs.push_back(action);
            return [polls = 0]() mutable -> std::optional<egraph::tui::RunResult> {
                if (++polls < 3) {
                    return std::nullopt;
                }
                return egraph::tui::RunResult{};
            };
        }};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(contains(planning, "Planning"));
    CHECK(contains(planning, "egraph exec --oneshot -u -N -D @installed"));
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().kind == Action::Kind::update);
    CHECK_FALSE(app.run_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "egraph exec finished");
}

TEST_CASE("without a way to preview, an action says so") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{12, 160, {character(U'U')}};
    egraph::tui::run(screen, app, ascii, {});
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->lines == std::vector<std::string>{"this egraph has no way to run actions"});
}

namespace {

using egraph::tui::Scope;

std::vector<std::string> shown_cpvs(const egraph::tui::App& app) {
    std::vector<std::string> cpvs;
    for (const auto id : app.list().shown) {
        cpvs.emplace_back(app.store().string(app.store().packages.at(id).cpv));
    }
    return cpvs;
}

void plan_page(egraph::tui::App& app) {
    REQUIRE(app.scope_plan_requested());
    auto job = app.start_scope_plan();
    CHECK(app.planning() == app.scope());
    app.finish_scope_plan(egraph::test::finish(std::move(job)));
    CHECK_FALSE(app.planning());
}

} // namespace

TEST_CASE("left and right turn the list's pages, each planning its set as exec would") {
    egraph::tui::App app{shared(glibmm_system()), true};
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(1), " @installed  @world  @system "));
    CHECK(std::ranges::contains(egraph::tui::view_keys(app, ascii),
                                egraph::tui::Hint{.key = "h/l", .meaning = "page", .bar = false}));
    CHECK(app.scope() == Scope::installed);
    CHECK_FALSE(app.scope_plan_requested());
    CHECK(shown_cpvs(app) == std::vector<std::string>{"app-misc/glibmm-1", "app-misc/loose-1"});

    app.handle(key(KeyKind::right));
    CHECK(app.scope() == Scope::world);
    CHECK(app.pages().empty());
    CHECK(app.scope_plan_requested());
    CHECK(shown_cpvs(app).empty());
    // Nothing to show of a plan not made yet.
    app.handle(character(U'p'));
    CHECK_FALSE(app.planned().has_value());
    plan_page(app);
    // Nothing keeps loose, so -uDN @world leaves it.
    CHECK(shown_cpvs(app) == std::vector<std::string>{"app-misc/glibmm-1"});
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    CHECK(app.plan().merges.size() == 3);
    app.handle(key(KeyKind::escape));

    app.handle(character(U'U'));
    REQUIRE(app.preview_requested().has_value());
    CHECK(egraph::tui::action_arguments(*app.preview_requested()) ==
          std::vector<std::string>{"exec", "--oneshot", "-u", "-N", "-D", "@world"});
    app.finish_preview({.ready = false, .out = "", .err = ""});
    app.handle(key(KeyKind::escape));

    // An empty @system asks for nothing.
    app.handle(character(U'l'));
    CHECK(app.scope() == Scope::system);
    plan_page(app);
    CHECK(shown_cpvs(app).empty());
    CHECK(app.plan().merges.empty());
    app.handle(key(KeyKind::right));
    CHECK(app.scope() == Scope::system);

    // Each page keeps its plan.
    app.handle(character(U'h'));
    CHECK_FALSE(app.scope_plan_requested());
    CHECK(shown_cpvs(app) == std::vector<std::string>{"app-misc/glibmm-1"});
    app.handle(key(KeyKind::left));
    app.handle(key(KeyKind::left));
    CHECK(app.scope() == Scope::installed);
    CHECK(shown_cpvs(app) == std::vector<std::string>{"app-misc/glibmm-1", "app-misc/loose-1"});
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().size() == 1);
}

TEST_CASE("a page's plan made for stores since replaced is dropped and made again") {
    egraph::tui::App app{shared(glibmm_system()), true};
    app.handle(key(KeyKind::right));
    auto job = app.start_scope_plan();
    app.finish_refresh(shared(glibmm_system("2")));
    app.finish_scope_plan(egraph::test::finish(std::move(job)));
    CHECK_FALSE(app.scope_planned(Scope::world));
    plan_page(app);
    CHECK(app.scope_planned(Scope::world));
    CHECK(shown_cpvs(app) == std::vector<std::string>{"app-misc/glibmm-1"});
}

TEST_CASE("run makes a page's plan in the background, saying so meanwhile") {
    egraph::tui::App app{shared(glibmm_system()), true};
    FakeScreen screen{16, 120, {key(KeyKind::right)}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    CHECK(contains(screen.text(), "planning @world"));
    // Its spinner turns.
    CHECK(screen.timeouts.back() == egraph::tui::wait_interval);
}

TEST_CASE("the corner shows the status file's plan of @world") {
    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    egraph::tui::StatusShown shown{
        .status = {.written = now,
                   .stores = {},
                   .counts = {.upgrades = 3, .rebuilds = 2, .held = 1},
                   .repositories = {{.name = "local"},
                                    {.name = "gentoo", .synced = now - hours{1}},
                                    {.name = "guru", .synced = now - minutes{5}}},
                   .lines = {}},
        .current = true};
    egraph::tui::App app{both(), true};
    FakeScreen screen{10, 120, {}};
    egraph::tui::run(screen, app, ascii, {.check = no_check, .status = [&shown] { return shown; }});
    REQUIRE(app.status().has_value());
    CHECK(screen.line(1).ends_with("@world U3 R2 H1  gentoo synced 1 hour ago "));
    shown.current = false;
    shown.status.counts = {.refused = true};
    app.finish_status(shown);
    FakeScreen wide{10, 160, {}};
    egraph::tui::draw(wide, app, ascii);
    INFO(wide.line(1));
    CHECK(
        contains(wide.line(1),
                 "@world + up to date ! refused  gentoo synced 1 hour ago  older than the stores"));
    FakeScreen narrow{10, 60, {}};
    egraph::tui::draw(narrow, app, ascii);
    CHECK_FALSE(contains(narrow.line(1), "@world +"));
}

namespace {

egraph::tui::NoticesShown two_notices() {
    using egraph::NoticeKind;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return {.notices = {{.kind = NoticeKind::glsa,
                         .key = "glsa:202609-03",
                         .title = "GLSA 202609-03: libfoo: heap overflow",
                         .detail = {"affects dev-libs/b-1, fixed in >=dev-libs/b-2"},
                         .fingerprint = "1",
                         .since = now - std::chrono::hours{3}},
                        {.kind = NoticeKind::news,
                         .key = "news:gentoo/2026-09-01-x",
                         .title = "Profile 23.0 is here",
                         .detail = {"gentoo news 2026-09-01-x", "eselect news read to read it"},
                         .fingerprint = "2",
                         .since = now}},
            .set_aside = 1};
}

} // namespace

TEST_CASE("the notices page follows the sets, its tab counting them") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK_FALSE(contains(screen.line(1), "notices"));
    app.finish_notices(two_notices());
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(1), " @installed  @world  @system  notices 2 "));

    for (int turn = 0; turn < 3; ++turn) {
        CHECK_FALSE(app.on_notices());
        app.handle(key(KeyKind::right));
    }
    REQUIRE(app.on_notices());
    // The last page.
    app.handle(key(KeyKind::right));
    CHECK(app.on_notices());
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(screen.line(0), "2 notices  1 set aside"));
    CHECK(contains(text, "glsa      GLSA 202609-03: libfoo: heap overflow"));
    CHECK(contains(text, "news      Profile 23.0 is here"));
    CHECK(contains(text, "3 hours ago"));
    // The selected notice's detail, below.
    CHECK(contains(text, "affects dev-libs/b-1, fixed in >=dev-libs/b-2"));
    CHECK_FALSE(contains(text, "eselect news read"));
    CHECK(contains(screen.line(15), " x dismiss  z later  "));

    app.handle(character(U'j'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "eselect news read to read it"));
    CHECK_FALSE(contains(screen.text(), "fixed in >=dev-libs/b-2"));

    app.handle(character(U'h'));
    CHECK_FALSE(app.on_notices());
    CHECK(app.scope() == Scope::system);
}

TEST_CASE("enter on a notice previews its action, or asks before a sync") {
    using egraph::NoticeKind;
    egraph::tui::App app{both(), true};
    auto shown = two_notices();
    shown.notices.front().packages = {"dev-libs/b-1"};
    shown.notices.push_back(
        {.kind = NoticeKind::stale, .key = "stale:gentoo", .title = "gentoo synced 9 days ago"});
    app.finish_notices(shown);
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    REQUIRE(app.on_notices());
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), " enter update  x dismiss  z later  "));

    app.handle(key(KeyKind::enter));
    REQUIRE(app.preview_requested().has_value());
    CHECK(*app.preview_requested() ==
          Action{.kind = Action::Kind::update, .targets = {"dev-libs/b"}});
    confirm(app);
    REQUIRE(app.run_requested().has_value());
    // In the emerge view, as any run; leaving it goes back to the notices.
    CHECK(app.watched().has_value());
    app.finish_run(egraph::tui::RunResult{});
    app.handle(character(U'x'));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.watched().has_value());
    CHECK(app.on_notices());

    // A news item from a notices file that does not name its text: nothing to do.
    app.handle(character(U'j'));
    egraph::tui::draw(screen, app, ascii);
    CHECK_FALSE(contains(screen.line(15), "enter"));
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.preview_requested().has_value());
    CHECK_FALSE(app.dialog().has_value());

    app.handle(character(U'j'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), " enter sync  "));
    app.handle(key(KeyKind::enter));
    // Nothing to preview: a sync is asked about straight away.
    CHECK_FALSE(app.preview_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->question);
    CHECK(app.dialog()->title == "Run egraph sync?");
    CHECK(app.dialog()->lines == std::vector<std::string>{"egraph sync gentoo"});
    app.handle(character(U'y'));
    REQUIRE(app.run_requested().has_value());
    CHECK(*app.run_requested() == Action{.kind = Action::Kind::sync, .targets = {"gentoo"}});
    app.finish_run(egraph::tui::RunResult{});
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "egraph sync finished");
}

TEST_CASE("enter on a news item shows it, and closing it marks it read") {
    egraph::tui::App app{both(), true};
    auto shown = two_notices();
    shown.notices.at(1).file = "/repo/metadata/news/2026-09-01-x/2026-09-01-x.en.txt";
    app.finish_notices(shown);
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    app.handle(character(U'j'));
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), " enter read  x dismiss  z later  "));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.news_requested().has_value());
    CHECK(app.news_requested()->key == "news:gentoo/2026-09-01-x");
    // Nothing else while it is read.
    app.handle(character(U'x'));
    CHECK_FALSE(app.notice_change_requested().has_value());

    std::vector<std::string> text{"Title: Profile 23.0 is here", "", "Switch profiles."};
    for (int line = 0; line < 30; ++line) {
        text.push_back(std::format("line {}", line));
    }
    app.finish_news(text);
    CHECK_FALSE(app.news_requested().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "Profile 23.0 is here");
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Switch profiles."));
    CHECK(contains(screen.text(), " any key marks it read "));
    // Scrolling is not closing.
    app.handle(character(U'j'));
    REQUIRE(app.dialog().has_value());
    CHECK_FALSE(app.notice_change_requested().has_value());
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.dialog().has_value());
    REQUIRE(app.notice_change_requested().has_value());
    CHECK(app.notice_change_requested()->read);
    CHECK(app.notice_change_requested()->notice.key == "news:gentoo/2026-09-01-x");
    app.finish_notice_change({});
    REQUIRE(app.notices()->notices.size() == 1);
    // Read, not set aside.
    CHECK(app.notices()->set_aside == 1);
}

TEST_CASE("a news item that cannot be marked read is set aside instead") {
    egraph::tui::App app{both(), true};
    auto shown = two_notices();
    shown.notices.at(1).file = "/repo/metadata/news/2026-09-01-x/2026-09-01-x.en.txt";
    app.finish_notices(shown);
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    app.handle(character(U'j'));
    app.handle(key(KeyKind::enter));
    app.finish_news(std::unexpected(std::string{"No such file or directory"}));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->title == "Cannot read the news item");
    // Closing the error marks nothing.
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.notice_change_requested().has_value());

    app.handle(key(KeyKind::enter));
    app.finish_news(std::vector<std::string>{"text"});
    app.handle(key(KeyKind::escape));
    REQUIRE(app.notice_change_requested().has_value());
    app.finish_notice_change(std::unexpected(std::string{"Permission denied"}));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "Set aside for you alone");
    CHECK(contains(app.dialog()->lines.at(1), "Permission denied"));
    REQUIRE(app.notice_change_requested().has_value());
    CHECK_FALSE(app.notice_change_requested()->read);
    CHECK_FALSE(app.notice_change_requested()->later.has_value());
    app.finish_notice_change({});
    REQUIRE(app.notices()->notices.size() == 1);
    CHECK(app.notices()->set_aside == 2);
}

TEST_CASE("enter on a masked package opens its page, or says it is gone") {
    using egraph::NoticeKind;
    egraph::tui::App app{both(), true};
    const auto cpv = std::string{app.store().string(app.store().packages.at(1).cpv)};
    egraph::tui::NoticesShown shown{.notices = {{.kind = NoticeKind::masked,
                                                 .key = "masked:" + cpv,
                                                 .title = cpv + " is masked",
                                                 .packages = {cpv}},
                                                {.kind = NoticeKind::masked,
                                                 .key = "masked:x/gone-1",
                                                 .title = "x/gone-1 is masked",
                                                 .packages = {"x/gone-1"}}},
                                    .set_aside = 0};
    app.finish_notices(shown);
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    REQUIRE(app.on_notices());
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), " enter open  "));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 1);
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.on_notices());

    app.handle(character(U'j'));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "x/gone-1 is no longer installed");
}

namespace {

egraph::tui::NoticesShown config_notice() {
    return {.notices = {{.kind = egraph::NoticeKind::config,
                         .key = "config",
                         .title = "1 configuration file has updates waiting",
                         .detail = {"/etc/foo.conf"},
                         .fingerprint = "/etc/._cfg0000_foo.conf"}},
            .set_aside = 0};
}

} // namespace

TEST_CASE("enter on the configuration notice asks for dispatch-conf, and says how it failed") {
    egraph::tui::App app{both(), true};
    app.finish_notices(config_notice());
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    REQUIRE(app.on_notices());
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), " enter dispatch-conf  x dismiss  "));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.dispatch_conf_requested());
    // Nothing else meanwhile.
    app.handle(character(U'x'));
    CHECK_FALSE(app.notice_change_requested().has_value());

    app.finish_dispatch_conf(0);
    CHECK_FALSE(app.dispatch_conf_requested());
    CHECK_FALSE(app.dialog().has_value());

    app.handle(key(KeyKind::enter));
    app.finish_dispatch_conf(2);
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->title == "dispatch-conf exited with status 2");
    app.handle(key(KeyKind::escape));

    app.handle(key(KeyKind::enter));
    app.finish_dispatch_conf(std::unexpected(std::string{"cannot run dispatch-conf: not found"}));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "Cannot run dispatch-conf");
    CHECK(app.dialog()->lines == std::vector<std::string>{"cannot run dispatch-conf: not found"});
}

TEST_CASE("enter on the configuration check's notice runs the check, esc goes back") {
    egraph::tui::App app{both(), true};
    app.finish_notices(
        egraph::tui::NoticesShown{.notices = {{.kind = egraph::NoticeKind::check,
                                               .key = "check",
                                               .title = "Configuration check: 1 warning",
                                               .detail = {"/etc/portage/package.use: 1 warning"},
                                               .fingerprint = "0123456789abcdef"}},
                                  .set_aside = 0});
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    REQUIRE(app.on_notices());
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), " enter check  x dismiss  "));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.command_requested() == std::optional<std::string>{"config check"});
    app.finish_command({.exit = egraph::Exit::ok,
                        .out = "/etc/portage/package.use\t3\twarning\tno-effect\tx/y\tz\t"
                               "already set\n",
                        .err = {}});
    REQUIRE(app.output().has_value());
    CHECK(app.output()->rows.size() == 1);
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.output().has_value());
    CHECK(app.on_notices());
}

TEST_CASE("run steps aside for dispatch-conf, then reads the notices again") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{
        16,
        120,
        {key(KeyKind::right), key(KeyKind::right), key(KeyKind::right), key(KeyKind::enter)}};
    int reads = 0;
    bool suspended_while_run = false;
    const auto stopped =
        egraph::tui::run(screen, app, ascii,
                         {.check = no_check,
                          .notices =
                              [&reads] {
                                  ++reads;
                                  return reads == 1 ? std::optional{config_notice()}
                                                    : std::optional{egraph::tui::NoticesShown{}};
                              },
                          .dispatch_conf = [&]() -> std::expected<int, std::string> {
                              suspended_while_run = screen.suspended;
                              return 0;
                          }});
    CHECK_FALSE(stopped.has_value());
    CHECK(suspended_while_run);
    CHECK(screen.suspends == 1);
    CHECK_FALSE(screen.suspended);
    CHECK(reads == 2);
    CHECK(app.notices()->notices.empty());
}

TEST_CASE("run stops when the terminal cannot be taken back from dispatch-conf") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{16,
                      120,
                      {key(KeyKind::right), key(KeyKind::right), key(KeyKind::right),
                       key(KeyKind::enter), character(U'j')}};
    screen.resumable = false;
    const auto stopped =
        egraph::tui::run(screen, app, ascii,
                         {.check = no_check,
                          .notices = [] { return std::optional{config_notice()}; },
                          .dispatch_conf = []() -> std::expected<int, std::string> { return 0; }});
    CHECK(stopped == "cannot start the terminal interface");
    // Nothing drawn or read after.
    CHECK(screen.keys_left());
}

TEST_CASE("without a way to run dispatch-conf, run says so and keeps the terminal") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{
        16,
        120,
        {key(KeyKind::right), key(KeyKind::right), key(KeyKind::right), key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii,
                     {.check = no_check, .notices = [] { return std::optional{config_notice()}; }});
    CHECK(screen.suspends == 0);
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "Cannot run dispatch-conf");
}

TEST_CASE("the interface can open on the notices page, once there are notices") {
    egraph::tui::App app{both(), true};
    app.open_notices();
    CHECK_FALSE(app.on_notices());
    app.finish_notices(two_notices());
    CHECK(app.on_notices());
    // Only the first time.
    app.handle(key(KeyKind::left));
    app.finish_notices(two_notices());
    CHECK_FALSE(app.on_notices());

    egraph::tui::App loaded{both(), true};
    loaded.finish_notices(two_notices());
    loaded.open_notices();
    CHECK(loaded.on_notices());
    // None: the list.
    egraph::tui::App none{both(), true};
    none.open_notices();
    none.finish_notices(std::nullopt);
    CHECK_FALSE(none.on_notices());
}

TEST_CASE("the notices page needs no evaluated store") {
    egraph::tui::App app{egraph::Stores{.installed = sample(), .evaluated = {}}, true};
    REQUIRE_FALSE(app.has_evaluated());
    FakeScreen screen{16, 120, {}};
    app.handle(key(KeyKind::right));
    CHECK_FALSE(app.on_notices());
    app.finish_notices(egraph::tui::NoticesShown{});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(1), " packages  notices 0 "));
    app.handle(key(KeyKind::right));
    REQUIRE(app.on_notices());
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Nothing needs you"));
    CHECK_FALSE(contains(screen.line(15), "x dismiss"));
    // Nothing to set aside.
    app.handle(character(U'x'));
    app.handle(character(U'z'));
    CHECK_FALSE(app.notice_change_requested().has_value());
    CHECK_FALSE(app.putting_off());
    app.handle(key(KeyKind::left));
    CHECK_FALSE(app.on_notices());
}

TEST_CASE("x dismisses the selected notice, and z puts it off for the time chosen") {
    egraph::tui::App app{both(), true};
    app.finish_notices(two_notices());
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    REQUIRE(app.on_notices());

    app.handle(character(U'x'));
    REQUIRE(app.notice_change_requested().has_value());
    CHECK(app.notice_change_requested()->notice.key == "glsa:202609-03");
    CHECK_FALSE(app.notice_change_requested()->later.has_value());
    app.finish_notice_change({});
    CHECK_FALSE(app.notice_change_requested().has_value());
    REQUIRE(app.notices().has_value());
    REQUIRE(app.notices()->notices.size() == 1);
    CHECK(app.notices()->notices.front().key == "news:gentoo/2026-09-01-x");
    CHECK(app.notices()->set_aside == 2);

    FakeScreen screen{16, 120, {}};
    app.handle(character(U'z'));
    CHECK(app.putting_off());
    egraph::tui::draw(screen, app, ascii);
    auto text = screen.text();
    INFO(text);
    CHECK(contains(text, " Put off "));
    CHECK(contains(text, "1  an hour"));
    CHECK(contains(text, "3  a week"));
    CHECK(contains(text, " esc cancel "));
    CHECK(contains(screen.line(15), " 1 an hour  2 a day  3 a week  esc cancel  "));
    // Neither the keys list nor the pages while choosing.
    app.handle(character(U'?'));
    CHECK_FALSE(app.keys_shown());
    CHECK(app.putting_off());
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.putting_off());
    CHECK_FALSE(app.notice_change_requested().has_value());

    app.handle(character(U'z'));
    app.handle(character(U'2'));
    CHECK_FALSE(app.putting_off());
    REQUIRE(app.notice_change_requested().has_value());
    CHECK(app.notice_change_requested()->later == std::chrono::days{1});
    // A failure keeps the notice and says why.
    app.finish_notice_change(std::unexpected(std::string{"read-only file system"}));
    CHECK(app.notices()->notices.size() == 1);
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->lines == std::vector<std::string>{"read-only file system"});
}

TEST_CASE("notices read again keep the cursor on its notice, and the page goes with them") {
    egraph::tui::App app{both(), true};
    app.finish_notices(two_notices());
    for (int turn = 0; turn < 3; ++turn) {
        app.handle(key(KeyKind::right));
    }
    app.handle(character(U'j'));
    CHECK(app.notice_cursor().at == 1);
    auto again = two_notices();
    std::ranges::reverse(again.notices);
    app.finish_notices(again);
    CHECK(app.notice_cursor().at == 0);
    // Gone, it leaves the cursor where it was, within the list.
    app.handle(character(U'j'));
    again.notices.pop_back();
    app.finish_notices(again);
    CHECK(app.notice_cursor().at == 0);
    app.finish_notices(std::nullopt);
    CHECK_FALSE(app.on_notices());
    CHECK(app.scope() == Scope::system);
}

TEST_CASE("run reads the notices with the status and sets them aside through its services") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{16,
                      120,
                      {key(KeyKind::right), key(KeyKind::right), key(KeyKind::right),
                       character(U'z'), character(U'1'), character(U'x')}};
    std::vector<std::pair<std::string, std::optional<std::chrono::seconds>>> set;
    egraph::tui::run(screen, app, ascii,
                     {.check = no_check,
                      .notices = [] { return std::optional{two_notices()}; },
                      .set_aside = [&set](const egraph::Notice& notice,
                                          std::optional<std::chrono::seconds> later)
                          -> std::expected<void, std::string> {
                          set.emplace_back(notice.key, later);
                          return {};
                      }});
    CHECK(set == std::vector<std::pair<std::string, std::optional<std::chrono::seconds>>>{
                     {"glsa:202609-03", std::chrono::hours{1}},
                     {"news:gentoo/2026-09-01-x", std::nullopt}});
}

TEST_CASE("a notice cannot be set aside by an egraph with no way to") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{
        16, 120, {key(KeyKind::right), key(KeyKind::right), key(KeyKind::right), character(U'x')}};
    egraph::tui::run(screen, app, ascii,
                     {.check = no_check, .notices = [] { return std::optional{two_notices()}; }});
    REQUIRE(app.dialog().has_value());
    CHECK(app.notices()->notices.size() == 2);
}

TEST_CASE("run reads a news item and marks it read through its services") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{16,
                      120,
                      {key(KeyKind::right), key(KeyKind::right), key(KeyKind::right),
                       character(U'j'), key(KeyKind::enter), key(KeyKind::escape)}};
    std::vector<std::string> asked;
    std::vector<std::string> read;
    egraph::tui::run(
        screen, app, ascii,
        {.check = no_check,
         .notices =
             [] {
                 auto shown = two_notices();
                 shown.notices.at(1).file = "/repo/news.en.txt";
                 return std::optional{shown};
             },
         .news_text = [&asked](const egraph::Notice& notice)
             -> std::expected<std::vector<std::string>, std::string> {
             asked.push_back(notice.file);
             return std::vector<std::string>{"Switch profiles."};
         },
         .mark_read = [&read](const egraph::Notice& notice) -> std::expected<void, std::string> {
             read.push_back(notice.key);
             return {};
         }});
    CHECK(asked == std::vector<std::string>{"/repo/news.en.txt"});
    CHECK(read == std::vector<std::string>{"news:gentoo/2026-09-01-x"});
}
